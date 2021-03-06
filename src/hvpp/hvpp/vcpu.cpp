#include "vcpu.h"
#include "vmexit.h"

#include "lib/assert.h"
#include "lib/log.h"
#include "lib/mm.h"

#include <iterator> // std::end()

#include "vcpu.inl"

namespace hvpp {

//
// Public
//

vcpu_t::vcpu_t(vmexit_handler& handler) noexcept
  //
  // Initialize VMXON region and VMCS.
  //
  : vmxon_{}
  , vmcs_{}

  //
  // This is not really needed.
  // MSR bitmaps and I/O bitmaps are actually copied here from
  // user-provided buffers (via msr_bitmap() and io_bitmap() methods)
  // before they are enabled.
  //
  // , msr_bitmap_{}
  // , io_bitmap_{}

  , handler_ { handler }

  //
  // Signalize that this VCPU is turned off.
  //
  , state_{ state::off }

  //
  // Let EPT be uninitialized.
  // VM-exit handler is responsible for EPT setup.
  //
  , ept_{ nullptr }
  , ept_count_{ 0 }
  , ept_index_{ 0 }

  //
  // Initialize pending-interrupt FIFO queue.
  //
  , pending_interrupt_first_{ 0 }
  , pending_interrupt_count_{ 0 }

  //
  // Well, this is also not necessary.
  // This member is reset to "false" on each VM-exit in entry_host() method.
  //
  , suppress_rip_adjust_{ false }
{
  //
  // Fill out initial stack with garbage.
  //
  memset(stack_.data, 0xcc, sizeof(stack_));

  //
  // Reset guest and exit context.
  // This is not really needed, as they are overwritten anyway (in
  // entry_guest_()/entry_host_()), but since initialization is done
  // just once, it also doesn't hurt.
  //
  guest_context_.clear();
  exit_context_.clear();

  //
  // Assertions.
  //
  [this] ()
  {
    //
    // Sanity checks for offsets relative to the host/guest stack (see
    // guest_rsp() and host_rsp() methods).  These offsets are also hardcoded
    // in the vcpu.asm file.  If they are ever changed, the static_assert
    // should be hint to fix them in the vcpu.asm as well.
    //
    constexpr intptr_t VCPU_RSP                         =  offsetof(vcpu_t, stack_) + sizeof(vcpu_t::stack_);
    constexpr intptr_t VCPU_OFFSET                      =  -0x8000;   // -vcpu_stack_size
    constexpr intptr_t VCPU_LAUNCH_CONTEXT_OFFSET       =   0;
    constexpr intptr_t VCPU_EXIT_CONTEXT_OFFSET         =   144;      // sizeof(context);

    static_assert(VCPU_RSP + VCPU_OFFSET                == offsetof(vcpu_t, stack_));
    static_assert(VCPU_RSP + VCPU_LAUNCH_CONTEXT_OFFSET == offsetof(vcpu_t, guest_context_));
    static_assert(VCPU_RSP + VCPU_EXIT_CONTEXT_OFFSET   == offsetof(vcpu_t, exit_context_));
  };
}

vcpu_t::~vcpu_t() noexcept
{
  //
  // When destructor is called, we should be only in one of the states
  // metioned in the "assert".
  //
  // We can't be in:
  //   - "initializing", because "initializing" goes directly
  //     to "running" (on success) or "terminated" (on error)
  //   - "launching", because (as said above), "launching" goes
  //     directly to "running"
  //   - "terminating", because "terminating" goes directly to "terminated"
  //
  hvpp_assert(state_ == state::off ||
              state_ == state::running ||
              state_ == state::terminated);

  if (state_ == state::running)
  {
    stop();
  }
}

auto vcpu_t::start() noexcept -> error_code_t
{
  //
  // Launch of the VCPU is performed via similar principle as setjmp/longjmp:
  //   - Save current state here (guest_context_.capture() returns 0 if it's
  //     been called by original code - which is the same as state::off).
  //   - Call vcpu_t::vmx_enter(), which will enter VMX operation and set up VCMS.
  //   - Launch the VM.
  //     Note that vmlaunch() function should NOT return - the next instruction
  //     after vmlaunch should be at vcpu_t::entry_guest_() (vcpu.asm).
  //   - The guest will set guest_context_.rax = state::launching (see entry_guest())
  //     and perform guest_context_.restore() (see vcpu.asm).
  //     That will catapult us back here.
  //   - We'll set state to state::running and exit this function.
  //

  switch (static_cast<state>(guest_context_.capture()))
  {
    case state::off:
      if (auto err = vmx_enter())
      {
        //
        // There was either error with enabling VMX, setting up VMCS,
        // or calling vmexit_handler::setup().
        //
        return handle_vmx_enter_error(err);
      }

      //
      // Launch the VM (i.e.: execute "vmlaunch" instruction).
      // If succeeded, this function does NOT return.
      //
      vmx::vmlaunch();

      //
      // If we got here, it means the "vmlaunch" failed.
      //
      return handle_vmx_launch_error();

    case state::launching:
      //
      // The vcpu_t::entry_guest() successfully put this VCPU into
      // "launching" state and vcpu.asm called guest_context_.restore().
      // This means that guest is running properly.
      //
      state_ = state::running;
      return error_code_t{};

    default:
      //
      // We shouldn't get here.
      //
      hvpp_assert(0);
      return make_error_code_t(std::errc::permission_denied);
  }
}

void vcpu_t::stop() noexcept
{
  //
  // Calling this method on any other state than "running" is considered
  // error.
  //
  hvpp_assert(state_ == state::running);

  //
  // Signalize that this VCPU is terminating.
  //
  state_ = state::terminating;

  //
  // Notify the exit handler that we're about to terminate.
  // Exit handler should invoke VMEXIT in such way, that causes
  // handler to call vcpu_t::vmx_leave(); e.g. VMCALL with specific
  // index.
  //
  handler_.teardown(*this);

  //
  // Destroy EPT.
  //
  ept_disable();
}

auto vcpu_t::vmx_enter() noexcept -> error_code_t
{
  //
  // Enter VMX operation, invalidate EPT and VPID, load VMCS,
  // set VMCS fields and call handler's setup() method.
  //

  if (auto err = load_vmxon())
  { return err; }

  if (auto err = load_vmcs())
  { return err; }

  if (auto err = setup_host())
  { return err; }

  if (auto err = setup_guest())
  { return err; }

  //
  // #TODO: This function can fail, make it
  // return appropriate error_code_t.
  //

  handler_.setup(*this);

  return error_code_t{};
}

void vcpu_t::vmx_leave() noexcept
{
  //
  // This method must be called either:
  //   - when initialization fails
  //   - when VCPU is terminating
  //
  hvpp_assert(state_ == state::initializing ||
              state_ == state::terminating);

  //
  // If vmx_leave() is called in the initialization phase,
  // we don't have to fix-up GDTR/IDTR/CR3, because:
  //   - no VM-exit occured yet
  //   - guest_gdtr/guest_idtr/guest_cr3 may still be uninitialized
  //

  if (state_ != state::initializing)
  {
    //
    // Advance RIP before we exit VMX-root mode. This skips the "vmcall"
    // instruction.
    //
    exit_context_.rip += exit_instruction_length();

    //
    // When running in VMX-root mode, the processor will set limits of the
    // GDT and IDT to 0xffff (notice that there are no Host VMCS fields to
    // set these values).  This causes problems with PatchGuard, which will
    // believe that the GDTR and IDTR have been modified by malware, and
    // eventually crash the system.  Since we know what the original state
    // of the GDTR and IDTR was, simply restore it now.
    //
    write<gdtr_t>(guest_gdtr());
    write<idtr_t>(guest_idtr());

    //
    // Our callback routine may have interrupted an arbitrary user process,
    // and therefore not a thread running with a systemwide page directory.
    // Therefore if we return back to the original caller after turning off
    // VMX, it will keep our current "host" CR3 value which we set on entry
    // to the PML4 of the SYSTEM process.  We want to return back with the
    // correct value of the "guest" CR3, so that the currently executing
    // process continues to run with its expected address space mappings.
    //
    write<cr3_t>(guest_cr3());
  }

  //
  // Software can use the INVVPID instruction with the "all-context"
  // INVVPID type immediately after execution of the VMXON instruction
  // or immediately prior to execution of the VMXOFF instruction.
  // Either prevents potentially undesired retention of information
  // cached from paging structures between separate uses of VMX operation.
  // (ref: Vol3C[28.3.3.3(Guidelines for Use of the INVVPID Instruction)])
  //
  vmx::invvpid_all_contexts();

  //
  // Software can use the INVEPT instruction with the "all-context"
  // INVEPT type immediately after execution of the VMXON instruction
  // or immediately prior to execution of the VMXOFF instruction.
  // Either prevents potentially undesired retention of information
  // cached from EPT paging structures between separate uses of VMX operation.
  // (ref: Vol3C[28.3.3.4(Guidelines for Use of the INVEPT Instruction)])
  //
  vmx::invept_all_contexts();

  //
  // Turn off VMX-root mode on this logical processor.
  //
  // This instruction brings us back to ring 0 from the "ring -1".
  // Note that in non VMX-root mode, all VMX-instructions (incl.
  // invept/invvpid) raise #UD (invalid instruction opcode exception).
  //
  vmx::off();

  //
  // Disable VMX-enable bit so that other hypervisors (or us)
  // can load again.
  //
  auto cr4 = read<cr4_t>();
  cr4.vmx_enable = false;
  write<cr4_t>(cr4);

  //
  // Signalize that this VCPU has terminated.
  //
  state_ = state::terminated;
}

void vcpu_t::ept_enable(uint16_t count /* = 1 */) noexcept
{
  hvpp_assert(ept_ == nullptr && count > 0);

  //
  // Allocate all EPTs and initialize them.
  //
  ept_ = new ept_t[count];
  ept_count_ = count;
  hvpp_assert(ept_ != nullptr);

  //
  // Enable EPT.
  //
  auto procbased_ctls2 = processor_based_controls2();
  procbased_ctls2.enable_ept = true;
  processor_based_controls2(procbased_ctls2);

  //
  // Automatically select the first EPT.
  //
  ept_index(0);
}

void vcpu_t::ept_disable() noexcept
{
  if (!ept_)
  {
    return;
  }

  //
  // Disable EPT functionality.
  //
  if (state_ != state::terminated)
  {
    auto procbased_ctls2 = processor_based_controls2();
    procbased_ctls2.enable_ept = false;
    processor_based_controls2(procbased_ctls2);
  }

  //
  // Destroy EPT.
  //
  delete[] ept_;
  ept_ = nullptr;
}

auto vcpu_t::ept_index() noexcept -> uint16_t
{
  return ept_index_;
}

void vcpu_t::ept_index(uint16_t index) noexcept
{
  hvpp_assert(index < ept_count_);

  ept_pointer(ept_[index].ept_pointer());
  ept_index_ = index;
}

auto vcpu_t::ept(uint16_t index /* = 0 */) noexcept -> ept_t&
{
  hvpp_assert(index < ept_count_);

  return ept_[index];
}

auto vcpu_t::exit_context() noexcept -> context_t&
{
  return exit_context_;
}

void vcpu_t::suppress_rip_adjust() noexcept
{
  suppress_rip_adjust_ = true;
}

//
// Private
//

auto vcpu_t::handle_common_error(error_code_t err) noexcept -> error_code_t
{
  //
  // Signalize that this VCPU is terminated and leave the VMX operation.
  //
  state_ = state::terminated;
  vmx_leave();

  return err;
}

auto vcpu_t::handle_vmx_enter_error(error_code_t err) noexcept -> error_code_t
{
  return handle_common_error(err);
}

auto vcpu_t::handle_vmx_launch_error() noexcept -> error_code_t
{
  //
  // Fetch VMX error from the VMCS and print it to the debugger.
  //
  const auto instruction_error = exit_instruction_error();
  hvpp_error("error: %u (%s)\n",
              static_cast<uint32_t>(instruction_error),
              vmx::instruction_error_to_string(instruction_error));

  //
  // If debugger is attached, break into it.
  //
  if (debugger::is_enabled())
  {
    debugger::breakpoint();
  }

  return handle_common_error(make_error_code_t(std::errc::permission_denied));
}

auto vcpu_t::load_vmxon() noexcept -> error_code_t
{
  hvpp_assert(state_ == state::off);

  //
  // In VMX operation, processors may fix certain bits in CR0 and CR4
  // to specific values and not support other values.  VMXON fails if
  // any of these bits contains an unsupported value.
  // (ref: Vol3C[23.8(Restrictions on VMX Operation)])
  //
  write(vmx::adjust(read<cr0_t>()));
  write(vmx::adjust(read<cr4_t>()));

  //
  // Before executing VMXON, software allocates a region of memory
  // (called the VMXON region) that the logical processor uses to
  // support VMX operation.  The VMXON pointer must be 4-KByte aligned
  // (bits 11:0 must be zero).  Before executing VMXON, software should
  // write the VMCS revision identifier to the VMXON region.
  // (ref: Vol3C[24.11.5(VMXON Region)])
  //
  const auto vmx_basic = msr::read<msr::vmx_basic_t>();
  vmxon_.revision_id = vmx_basic.vmcs_revision_id;

  //
  // Enter VMX operation.
  //
  if (vmx::on(pa_t::from_va(&vmxon_)) != vmx::error_code::success)
  {
    return make_error_code_t(std::errc::permission_denied);
  }

  state_ = state::initializing;

  //
  // Software can use the INVVPID instruction with the "all-context"
  // INVVPID type immediately after execution of the VMXON instruction
  // or immediately prior to execution of the VMXOFF instruction.
  // Either prevents potentially undesired retention of information
  // cached from paging structures between separate uses of VMX operation.
  // (ref: Vol3C[28.3.3.3(Guidelines for Use of the INVVPID Instruction)])
  //
  vmx::invvpid_all_contexts();

  //
  // Software can use the INVEPT instruction with the "all-context"
  // INVEPT type immediately after execution of the VMXON instruction
  // or immediately prior to execution of the VMXOFF instruction.
  // Either prevents potentially undesired retention of information
  // cached from EPT paging structures between separate uses of VMX operation.
  // (ref: Vol3C[28.3.3.4(Guidelines for Use of the INVEPT Instruction)])
  //
  vmx::invept_all_contexts();

  return error_code_t{};
}

auto vcpu_t::load_vmcs() noexcept -> error_code_t
{
  hvpp_assert(state_ == state::initializing);

  const auto vmx_basic = msr::read<msr::vmx_basic_t>();
  vmcs_.revision_id = vmx_basic.vmcs_revision_id;

  //
  // Set VMCS to "clear" state and make the VMCS active.
  // See Vol3C[24(Virtual Machine Control Structures)] for more information.
  //
  if (vmx::vmclear(pa_t::from_va(&vmcs_)) != vmx::error_code::success ||
      vmx::vmptrld(pa_t::from_va(&vmcs_)) != vmx::error_code::success)
  {
    return make_error_code_t(std::errc::permission_denied);
  }

  return error_code_t{};
}

auto vcpu_t::setup_host() noexcept -> error_code_t
{
  hvpp_assert(state_ == state::initializing);

  //
  // Sets up what state will the CPU have when VM-exit is triggered.
  // Notice how these fields mainly consist of descriptor registers,
  // control registers, and segment registers.  This effectively allows us
  // to run hypervisor in completely separate address space from the OS.
  // We're not going to do it, though - we're going to mirror current CPU
  // state instead.  This setup is convenient for us, as we'll be able to
  // simply see NT Kernel memory, call its functions (callable from HIGH_LEVEL
  // IRQL only) and even see virtual address space of any process with simple
  // CR3 switch.
  //
  // Also notice how we're not setting other registers - such as GP registers
  // (RAX, RBX, ...) or SSE registers - these registers are preserved from the
  // guest.
  //
  const auto gdtr = read<gdtr_t>();
  const auto idtr = read<idtr_t>();

  //
  // Note that we're setting just base address of GDTR and IDTR.
  // The limit of these descriptors is fixed at 0xffff for VMX operations.
  //
  host_gdtr(gdtr);
  host_idtr(idtr);

  //
  // Note that we're setting just selectors (base address - except for FS and
  // GS), limit and access rights are not set).
  //
  host_cs(segment_t{ gdtr, read<cs_t>() });
  host_ds(segment_t{ gdtr, read<ds_t>() });
  host_es(segment_t{ gdtr, read<es_t>() });
  host_fs(segment_t{ gdtr, read<fs_t>() });
  host_gs(segment_t{ gdtr, read<gs_t>() });
  host_ss(segment_t{ gdtr, read<ss_t>() });
  host_tr(segment_t{ gdtr, read<tr_t>() });

  host_cr0(read<cr0_t>());
  host_cr3(read<cr3_t>());
  host_cr4(read<cr4_t>());

  //
  // We also have to set RSP and RIP values which will be set on every VM-exit.
  // Each VCPU has its own space reserved for stack, so it makes sense to set
  // RSP at its end (because RSP "grows" towards to zero).
  // RIP - aka instruction pointer - points to function which will be called
  // on every VM-exit.
  //
  host_rsp(reinterpret_cast<uint64_t>(std::end(stack_.data)));
  host_rip(reinterpret_cast<uint64_t>(&vcpu_t::entry_host_));

  return error_code_t{};
}

auto vcpu_t::setup_guest() noexcept -> error_code_t
{
  hvpp_assert(state_ == state::initializing);

  //
  // VPIDs provide a way for software to identify to the processor the
  // address spaces for different "virtual processors."  The processor
  // may use this identification to maintain concurrently information
  // for multiple address spaces in its TLBs and paging-structure caches,
  // even when non-zero PCIDs are not being used.
  // See Section 28.1 for details.
  // (ref: Vol3A[4.11.2(VMX Support for Address Translation)]
  //
  // Virtual-processor identifiers (VPIDs) introduce to VMX operation
  // a facility by which a logical processor may cache information for
  // multiple linear-address spaces.  When VPIDs are used, VMX transitions
  // may retain cached information and the logical processor switches to
  // a different linear-address space.
  //
  // VPIDs and PCIDs can be used concurrently.  When this is done, the
  // processor associates cached information with both a VPID and a PCID.
  // Such information is used only if the current VPID and PCID both match
  // those associated with the cached information.
  // (ref: Vol3C[28.1(Virtual Processor Identifiers (VPIDs))]
  //
  // TL;DR:
  //   Intel provides VM managing software a simple way how to manage TLB
  //   for multiple VMs.  Each VCPU of a particular VM can have associated
  //   unique VPID (VPID can be same for all VCPUs of one VM).
  //
  //   Imagine you have 2 or more VMs:
  //     - if you enable VPIDs, you don't have to worry that VM1 accidentaly
  //       fetches cached memory of VM2 (or even hypervisor itself)
  //     - if you don't enable VPIDs, CPU assigns VPID=0 to all operations
  //       (VMX root & VMX non-root) and flushes TLB on each transition for you
  //
  //   Also note that if you enable VPIDs, you can't assign VPID=0 to the
  //   guest VMCS because VPID=0 is reserved for VMX root operation.
  //
  vcpu_id(1);

  //
  // VMCS link pointer points to the shadow VMCS if VMCS shadowing is
  // enabled.  If VMCS shadowing is disabled, intel advises to set this
  // value to 0xFFFFFFFFFFFFFFFF.
  //
  vmcs_link_pointer(~0ull);

  //
  // By default we won't force VM-exit on any external interrupts.
  //
  pin_based_controls(msr::vmx_pinbased_ctls_t{});

  //
  // By default we want to use secondary processor based controls and
  // MSR bitmaps.  Few lines below we'll set zero-ed out MSR bitmap to
  // the VMCS to disable any MSR-related VM-exits.  This is because if
  // "use_msr_bitmaps" wouldn't be set, we would get VM-exit for each
  // MSR access (both read & write).  This is not always desirable.
  //
  msr::vmx_procbased_ctls_t procbased_ctls{};
  procbased_ctls.activate_secondary_controls = true;
  procbased_ctls.use_msr_bitmaps = true;
  processor_based_controls(procbased_ctls);

  //
  // By default we will enable VPID.
  // Also, enable RDTSCP, XSAVES and INVPCID instructions to be run by
  // guest (otherwise they would cause #UD).
  //
  msr::vmx_procbased_ctls2_t procbased_ctls2{};
  procbased_ctls2.enable_vpid = true;
  procbased_ctls2.enable_rdtscp = true;
  procbased_ctls2.enable_xsaves = true;
  procbased_ctls2.enable_invpcid = true;
  processor_based_controls2(procbased_ctls2);

  //
  // By default we want each VM-entry and VM-exit in 64bit mode.
  //
  msr::vmx_entry_ctls_t entry_ctls{};
  entry_ctls.ia32e_mode_guest = true;
  vm_entry_controls(entry_ctls);

  msr::vmx_exit_ctls_t exit_ctls{};
  exit_ctls.ia32e_mode_host = true;
  vm_exit_controls(exit_ctls);

  //
  // Set zero-ed out MSR bitmap.
  // Note that we would still get VM-exit for each MSR access, if the MSR ID
  // is out of following ranges:
  //   0x00000000 - 0x00001fff and
  //   0xc0000000 - 0xc0001fff
  //
  msr_bitmap(vmx::msr_bitmap_t{});

  //
  // By default set initial stack and initial instruction pointer to
  // VCPU's private area.
  // Note that guest and host share the same stack (see setup_host() method).
  // This isn't a problem, because both guest and host will NOT be running at
  // the same time on the same VCPU.
  //
  guest_rsp(reinterpret_cast<uint64_t>(std::end(stack_.data)));
  guest_rip(reinterpret_cast<uint64_t>(&vcpu_t::entry_guest_));

  return error_code_t{};
}

void vcpu_t::entry_host() noexcept
{
  hvpp_assert(state_ == state::running ||
              state_ == state::terminating);

  //
  // Reset RIP-adjust flag.
  //
  suppress_rip_adjust_ = false;

  //
  // Execute "fxsave" instruction.  This causes to save x87 state and SSE
  // state.  It includes x87 registers (st0-st7 / mm0-mm7), XMM registers
  // (xmm0-xmm15 in 64bit mode, xmm0-xmm7 in 32bit mode) and MXCSR register
  // (control and status information regarding SSE instructions).
  // More information in Vol1[10.5(FXSAVE AND FXRSTOR INSTRUCTIONS)].
  //
  // This is needed because especially in Release build (with optimizations
  // enabled), the compiler might generate code which uses SSE instructions
  // and registers.
  //
  // Because VM-exit might interrupt any process, we don't want to leave
  // these registers clobbered - the application which we've interrupted
  // most likely relies on them.  Therefore, at the end of this function,
  // we restore them back.
  //
  // Note that there exists newer pair of instructions "xsave" and "xrstor"
  // which is also capable (among other things) of saving/restoring AVX state.
  // But as long as we're not compiled with AVX support, fxsave/fxrstor should
  // be enough.
  //
  ia32_asm_fx_save(&fxsave_area_);

  {
    //
    // Because we're in VMX-root mode, we can't use the system allocator
    // (ExAllocatePoolWithTag/ExFreePoolWithTag).
    // This line will enable "custom allocator" that will be used whenever
    // "new"/"delete" operator is executed.
    // See lib/mm.cpp for more details.
    //
    mm::allocator_guard _;

    const auto captured_rsp    = exit_context_.rsp;
    const auto captured_rflags = exit_context_.rflags;

    {
      exit_context_.rsp    = guest_rsp();
      exit_context_.rip    = guest_rip();
      exit_context_.rflags = guest_rflags();

      //
      // WinDbg will show full callstack (hypervisor + interrupted application)
      // after these two lines are executed.
      // See vcpu.asm for more details.
      //
      // Note that machine_frame.rip is supposed to hold return address.
      // exit_instruction_length() is added to the guest_rip() to create
      // this value.
      //
      stack_.machine_frame.rip = exit_context_.rip + exit_instruction_length();
      stack_.machine_frame.rsp = exit_context_.rsp;

      {
        handler_.handle(*this);

        if (state_ == state::terminated)
        {
          //
          // At this point we're not in the VMX-root mode (vmxoff has been
          // executed) and we want to return control back to whomever caused
          // this VM-exit.
          //
          // Note that at this point, we can't call any VMX instructions,
          // as they would raise #UD (invalid opcode exception).
          //
          goto exit;
        }

        if (!suppress_rip_adjust_)
        {
          exit_context_.rip += exit_instruction_length();
        }
      }

      guest_rsp(exit_context_.rsp);
      guest_rip(exit_context_.rip);
      guest_rflags(exit_context_.rflags);
    }

    exit_context_.rflags = captured_rflags;
    exit_context_.rsp    = captured_rsp;
    exit_context_.rip    = reinterpret_cast<uint64_t>(&vmx::vmresume);
  }

exit:
  ia32_asm_fx_restore(&fxsave_area_);
}

void vcpu_t::entry_guest() noexcept
{
  // hvpp_assert(state_ == state::initializing);

  guest_context_.rax = static_cast<uint64_t>(state::launching);
}

}
