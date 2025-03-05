#pragma once
#include <windows.h>

#include "logger.hpp"
#include "helper.hpp"
#include "driver.hpp"
#include "cpudef.hpp"
#include "hook.hpp"
#include "nt.hpp"

#define KB( x ) ( ( size_t ) ( x ) << 10 )
#define MB( x ) ( ( size_t ) ( x ) << 20 )

//
// length of stub to scan
//
#define STUB_SCAN_LENGTH 0x20

//
// use this if you are lazy
// all you need is define prototype of the function
// Fixed the first issue, which resolved the problem of failing to successfully call parameterized kernel functions.
//
#define SYSCALL( function_name, ... ) \
	syscall::invoke< function_name >( \
		( void* )( helper::find_ntoskrnl_export( #function_name )) , __VA_ARGS__)

// this is huge structure to define here...
using PEPROCESS = PVOID;

using PsLookupProcessByProcessId = NTSTATUS( __fastcall* )(
	HANDLE    ProcessId,
	PEPROCESS* Process );

using PsGetProcessSectionBaseAddress = PVOID( __fastcall* )(
	PEPROCESS Process );

using PsGetCurrentProcessId = HANDLE( __fastcall* )( void );

using MmGetPhysicalAddress = PHYSICAL_ADDRESS( __fastcall* )(
	PVOID BaseAddress );

//
// our syscall handler built by assembly
// syscall number is at offset 0x4 and
// will be set by syscall::setup
// only supports x64
//
// 0x4C 0x8B 0xD1 0xB8 0xFF 0xFF 0x00 0x00 0x0F 0x05 0xC3
//                     ^^^^^^^^^
//
// 0:  4c 8b d1                mov    r10, rcx
// 3:  b8 ff ff 00 00          mov    eax, 0xffff ; syscall number
// 8:  0f 05                   syscall
// a:  c3                      ret
//
// syscall_handler --> KiSystemCall64 -->  [hooked internal syscall] --> [detour]
// |      USER      |                          KERNEL                           |
//
extern "C" void* syscall_handler();

namespace syscall
{
	//
	// this points to the desired hook syscall function
	// that mapped to our user virtual address
	//
	inline void* function;

  //
  // This pointer points to our real syscall_handler function. 
  // Some compilers may optimize this function, so when we call it, we don't directly execute the function, but instead first jump to its actual address before execution. 
  // Regardless of whether it is optimized or not, this member always points to the actual implementation of the function. 
  //
	inline void* real_syscall_handler;

	//
	// does certain syscall-hook found?
	//
	inline bool found;

	//
	// cache function stub got from ntoskrnl.exe rva
	//
	inline uint8_t stub[ STUB_SCAN_LENGTH ];
	inline uint16_t page_offset;

	//
	// any kernel code execution - anycall
	//
	template < class FnType, class ... Args >
	std::invoke_result_t< FnType, Args... > invoke(
		void* detour, Args ... augments )
	{
		//
		// void function cannot return
		//
		constexpr auto is_ret_type_void =
			std::is_same<
				std::invoke_result_t< FnType, Args... >, void >{};

		//
		// inline-hook against desired arbitrary syscall
		//
		hook::hook( syscall::function, detour, true );

		if constexpr ( is_ret_type_void )
		{
			//
			// invoke syscall
			//
			reinterpret_cast< FnType >( syscall_handler )( augments ... );
		}
		else
		{
			//
			// invoke syscall
			//
			const auto invoke_result =
				reinterpret_cast< FnType >( syscall_handler )( augments ... );

			//
			// unhook immediately
			//
			hook::unhook( syscall::function, true );

			return invoke_result;
		}

		//
		// unhook immediately
		//
		hook::unhook( syscall::function, true );
	}

	//
	// 检查系统调用钩子是否成功
	//
	bool validate()
	{
		uint32_t pid_from_hooked_syscall = 0;

		//
		// wow, PsGetCurrentProcessId returns this user process's pid,
		// if the syscall-hook is succeeded
		//
		pid_from_hooked_syscall = ( uint32_t )SYSCALL( PsGetCurrentProcessId );

		const bool is_syscall_ok = 
			pid_from_hooked_syscall == GetCurrentProcessId();

		LOG( "[?] PsGetCurrentProcessId:\033[0;105;30m%d\033[0m == \033[0;105;30m%d\033[0m:GetCurrentProcessId -> %s\n",
			pid_from_hooked_syscall,
			GetCurrentProcessId(),
			is_syscall_ok ? "\033[0;102;30mOK\033[0m" : "\033[0;101;30mINVALID\033[0m" );

		return is_syscall_ok;
	}

	bool probe_for_hook( const uint64_t mapped_va )
	{
		//
		// compare stub of destination of hook function
		//
		if ( memcmp(
			reinterpret_cast< void* >( mapped_va ),
			stub, STUB_SCAN_LENGTH ) == 0 )
		{
			//
			// we can't trust this yet
			//
			syscall::function = reinterpret_cast< void* >( mapped_va );

			//
			// validate by try hook and call
			//
			return syscall::validate();
		}

		return false;
	}

	bool scan_for_range( 
		const uint64_t start_pa, const uint64_t end_pa )
	{
		LOG( "[+] scanning for range [\033[0;103;30m0x%llX -> 0x%llX\033[0m]\n",
			start_pa, end_pa );

		const auto pa_size = start_pa + end_pa;
		
		//
		// lazy lambda definition
		//
		const auto iterator = [ & ]( 
			const uint64_t base, const size_t size = MB( 2 ) )
		{
			// just for logging
			uint32_t counter = 0;

			for ( auto current_page = base;
				current_page < base + size;
				current_page += PAGE_SIZE )
			{
				counter++;

				//
				// probe this page
				//
				if ( probe_for_hook( current_page ) )
				{
					LOG( "[+] stub found in range [\033[0;103;30m0x%llX -> 0x%llX\033[0m] and page \033[0;103;30m%d\033[0m\n",
						start_pa, end_pa, counter );
					return true;
				}
			}

			return false;
		};

		if ( pa_size <= MB( 2 ) )
		{
			const uint64_t mapped_va = driver::map_physical_memory(
				start_pa + page_offset, end_pa );

			if ( !mapped_va )
			{
				LOG( "[!] \033[0;101;30mfailed to map physical memory\033[0m\n" );
				return false;
			}

			if ( iterator( mapped_va, end_pa ) )
				return true;

			driver::unmap_physical_memory( mapped_va, end_pa );
			return false;
		}
		
		//
		// big page
		//
		const auto modulus = pa_size % MB( 2 );

		for ( auto part = start_pa;
			part < pa_size;
			part += MB( 2 ) )
		{
			const uint64_t mapped_va = driver::map_physical_memory(
				part + page_offset, MB( 2 ) );

			if ( !mapped_va )
			{
				LOG( "[!] \033[0;101;30mfailed to map physical memory\033[0m\n" );
				continue;
			}

			if ( iterator( mapped_va, MB( 2 ) ) )
				return true;

			driver::unmap_physical_memory( mapped_va, MB( 2 ) );
		}

		const uint64_t mapped_va =
			driver::map_physical_memory(
				pa_size - modulus + page_offset, modulus );

		if ( !mapped_va )
		{
			LOG( "[!] \033[0;101;30mfailed to map physical memory\033[0m\n" );
			return false;
		}

		if ( iterator( mapped_va, modulus ) )
			return true;

		driver::unmap_physical_memory( mapped_va, modulus );
		return false;
	}

	//
	// syscall-hook initialization
	//
	bool setup(
		const std::string_view hook_function_module_name, // module name the function contains
		const std::string_view hook_function_name )       // any desired hook function
	{
		// already initialized
		if ( syscall::found )
			return false;

		//
		// fetch physical memory ranges from registry
		//
		std::vector< PHYSICAL_ADDRESS_RANGE > pa_range_list;
		helper::query_physical_memory_ranges( pa_range_list );

		if ( !pa_range_list.size() )
		{
			LOG( "[!] \033[0;101;30mfailed to fetch physical memory ranges\033[0m\n" );
			LOG_ERROR();

			return false;
		}

		LOG( "[+] preparing our syscall handler...\n" );

		//
		// find syscall number from image
		//
		const uint16_t syscall_number = 
			helper::find_syscall_number( 
				hook_function_module_name, hook_function_name );

		if ( !syscall_number )
		{
			LOG( "[!] \033[0;101;30mfailed to find syscall number\033[0m\n" );
			LOG_ERROR();

			return false;
		}

		helper::PrintHex(
			"[+] this is our syscall handler: \033[0;100;30m", "\033[0m",
			syscall_handler, 11);

		real_syscall_handler = (BYTE*)syscall_handler;
    
    //
    // There is a jump, and the target address needs to be tracked. The reason for the jump here is due to compiler optimization of this function. 
    // Normally, when a function's address is taken, it directly corresponds to the function's implementation code, rather than a jump.
    // This is the second fix, which resolves the issue with the call chain.
    //
		if (*(BYTE*)syscall_handler == 0xE9) {
			DWORD offset = *(DWORD*)((BYTE*)syscall_handler + 1);
			real_syscall_handler = (BYTE*)syscall_handler + 5 + offset;
			helper::PrintHex(
				"[+] Wow it include e9(jmp) so this is our real syscall handler: \033[0;100;30m", "\033[0m",
				real_syscall_handler, 11);  // print real code
		}

		if (!hook::Copy_Memory(
			(void*)((uint64_t)real_syscall_handler + 0x4), // our syscall number offset is 0x4
			(void*)const_cast<uint16_t*>(&syscall_number), // the syscall number
			sizeof(uint16_t)))                             // size must be 0x2
		{
			LOG("[!] \033[0;101;30mfailed to set syscall number\033[0m\n");
			LOG_ERROR();
			return false;
		}

		LOG("[+] syscall number for %s (0x%X) is set\n",
			hook_function_name.data(), syscall_number);

		helper::PrintHex(
			"[+] prepared our syscall handler: \033[0;100;30m", "\033[0m",
			real_syscall_handler, 11);

		const SYSMODULE_RESULT ntoskrnl =
			helper::find_sysmodule_address( "ntoskrnl.exe" );

		std::string ntoskrnl_full_path = ntoskrnl.image_full_path;
		helper::replace_systemroot( ntoskrnl_full_path );

		if ( !ntoskrnl.base_address )
		{
			LOG( "[!] \033[0;101;30mfailed to locate ntoskrnl.exe\033[0m\n" );
			return false;
		}

		//
		// temporally buffer
		//
		uint8_t* our_ntoskrnl;

		our_ntoskrnl = reinterpret_cast< uint8_t* >(
			LoadLibrary( ntoskrnl_full_path.c_str() ) );

		if ( !our_ntoskrnl )
		{
			LOG( "[!] \033[0;101;30mfailed to map ntoskrnl.exe into our process\033[0m\n" );
			LOG_ERROR();

			return false;
		}

		LOG( "[+] ntoskrnl.exe is at 0x%llX (ourselves: 0x%p)\n",
			ntoskrnl.base_address, our_ntoskrnl );

		//
		// rva and page offset to the desired syscall function
		//
		const auto hook_function_rva =
			helper::find_ntoskrnl_export( hook_function_name, true /* as rva */ );

		if ( !hook_function_rva )
		{
			LOG( "[!] \033[0;101;30mfailed to locate %s in ntoskrnl.exe\033[0m\n",
				hook_function_name.data() );

			return false;
		}

		page_offset = hook_function_rva % PAGE_SIZE;

		LOG( "[+] hook function rva: 0x%llX\n", hook_function_rva );
		LOG( "[+] page offset: 0x%lX\n", page_offset );
		LOG( "[+] ntoskrnl.exe path: %s\n", ntoskrnl_full_path.c_str() );

		//
		// cache hook function stub to our buffer
		//
		memcpy(
			&stub[ 0 ],
			( void* )( our_ntoskrnl + hook_function_rva ),
			STUB_SCAN_LENGTH );

		FreeLibrary( ( HMODULE )our_ntoskrnl );

		helper::print_hex( 
			"[+] function stub: \033[0;100;30m", "\033[0m", 
			( void* )stub, STUB_SCAN_LENGTH);

		//
		// scan for every single physical memory ranges
		//
		for ( const auto& pa_range : pa_range_list )
		{
			if ( scan_for_range( pa_range.start_pa, pa_range.end_pa ) )
			{
				//
				// physical address of the syscall::function va
				//
				PHYSICAL_ADDRESS physical_address =
					syscall::invoke< MmGetPhysicalAddress >( 
						( void* )helper::find_ntoskrnl_export( "MmGetPhysicalAddress" ),
						syscall::function );

				LOG( "[+] %s found at \033[0;103;30m0x%llX\033[0m\n",
					hook_function_name.data(),
					syscall::function, physical_address.QuadPart );

				syscall::found = true;
				break;
			}
		}

		if ( !syscall::found )
		{
			LOG( "[!] \033[0;101;30msyscall was not found\033[0m\n" );
			return false;
		}

		return true;
	}
} // namespace syscall
