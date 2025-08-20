# usbip-win2

> [!IMPORTANT] 
> This repo has been de facto superseded by [usbip-win2](https://github.com/vadimgrn/usbip-win2).

# USB/IP for Windows

- This project aims to support both a USB/IP server and a client on Windows platform.


## Build

### Notes
- Build is tested on Windows 10 x64 and the projects are configured for this target by default.
- x86/x64 platforms should be supported. However, we don't have an x86 setup for testing at the moment.
- For Windows 7 users, change `usbip_stub` and `usbip_vhci` projects' Target OS version to `Windows 7`.
  - Right-click on the `Project > Properties > Driver Settings > Target OS version > Windows 7`
  - The recent tag version which supports is v0.1.0. All versions after v0.1.0 do not support Windows 7 or 8 because usbip-win started to use kernel libraries of Windows 10.

### Build Tools
- Visual Studio 2019 Community (v142)
  - Build with VS 2017 (v141) is also possible if Platform Toolset in Setting is configured to v141.
- Windows SDK 10.0.18362.0 (recommended)
  - VS 2019 (v142): requires &gt;= 10.0.18xxx
  - VS 2017 (v141): requires &gt;= 10.0.17xxx
- Windows Driver Kit Windows 10, version 1903 (10.0.18362)
  - WDK 10.0.17134 (1803), 10.0.17763 (1809) and 10.0.18346 are also tested.

### Build Process
- Open `usbip_win.sln`
- If VS 2017 is used, SDK version for userspace projects (`usbip`, `usbip_common`, `usbipd`, `stubctl`) should be adjusted.
- Set certificate driver signing for `usbip_stub` and `usbip_vhci` projects
  - Right-click on the `Project > Properties > Driver Signing > Test Certificate`
  - Browse to `driver/usbip_test.pfx` (password: usbip)
- Build solution or desired project
- All output files are created under {Debug,Release}/{x64,x86} folder.

## Install

### Windows USB/IP server
- Prepare a Linux machine as a USB/IP client or Windows usbip-win VHCI client (tested on Ubuntu 16.04 with kernel 4.15.0-29 - USB/IP kernel module crash was observed on some other versions)
  - `# modprobe vhci-hcd`
- Install USB/IP test certificate
  - Install `driver/usbip_test.pfx` (password: usbip)
  - Certificate should be installed into
    1. "Trusted Root Certification Authority" in "Local Computer" (not current user) *and*
    2. "Trusted Publishers" in "Local Computer" (not current user)
- Enable test signing
  - `> bcdedit.exe /set TESTSIGNING ON`
  - reboot the system to apply
- Copy `usbip.exe`, `usbipd.exe`, `usb.ids`, `usbip_stub.sys`, `usbip_stub.inx` into a folder in target machine
  - You can find `usbip.exe`, `usbipd.exe`, `usbip_stub.sys` in the output folder after build or on [release](https://github.com/cezanne/usbip-win/releases) page.
  - `userspace/usb.ids`
  - `driver/stub/usbip_stub.inx`
- Find USB Device ID
  - You can get id from usbip listing
    - `> usbip.exe list -l`
  - Bus id is always 1. So output from `usbip.exe` listing is shown as:
```
usbip.exe list -l
 - busid 1-59 (045e:00cb)
   Microsoft Corp. : Basic Optical Mouse v2.0 (045e:00cb)
 - busid 1-30 (80ee:0021)
   VirtualBox : USB Tablet (80ee:0021)
```
- Bind USB device to usbip stub
  - The next command replaces the existing function driver with usbip stub driver
    - This should be executed using administrator privilege
    - `usbip_stub.inx` and `usbip_stub.sys` files should be in the same folder as `usbip.exe`
  - `> usbip.exe bind -b 1-59`
- Run `usbipd.exe`
  - `> usbipd.exe -d -4`
  - TCP port `3240` should be allowed by firewall
- Attach USB/IP device on Linux machine
  - `# usbip attach -r <usbip server ip> -b 1-59`

### Windows USB/IP client
- Currently, there are 2 versions for a VHCI driver with different installation procedures:
  - `vhci(wdm)`: original version, implemented via WDM (Windows Driver Model);
  - `vhci(ude)`: newly developed version to fully support USB applications and implemented via UDE (USB Device Emulation) which is MS provided USB virtualization framework over KMDF (Kernel-Model Driver Framework).
- Prepare a Linux machine as a USB/IP server or Windows usbip-win stub server - (tested on Ubuntu 16.04 (kernel 4.15.0-29), 18.04, 20.04)
  - `# modprobe usbip-host`
  - You can use virtual [usbip-vstub](https://github.com/cezanne/usbip-vstub) as a stub server
- Run usbipd on a USB/IP server (Linux)
  - `# usbipd -4 -d`
- Install USB/IP test certificate
  - Install `driver/usbip_test.pfx` (password: usbip)
  - Certificate should be installed into
    1. "Trusted Root Certification Authority" in "Local Computer" (not current user) *and*
    2. "Trusted Publishers" in "Local Computer" (not current user)
- Enable test signing
  - `> bcdedit.exe /set TESTSIGNING ON`
  - reboot the system to apply
- Copy VHCI driver files into a folder in target machine
  - If you're testing `vhci(ude)`, copy `usbip.exe`, `usbip_vhci_ude.sys`, `usbip_vhci_ude.inf`, `usbip_vhci_ude.cat` into a folder in target machine;
  - If you're testing `vhci(wdm)`, copy `usbip.exe`, `usbip_vhci.sys`, `usbip_vhci.inf`, `usbip_root.inf`, `usbip_vhci.cat` into a folder in target machine;
  - You can find all files in output folder after build or on [release](https://github.com/cezanne/usbip-win/releases) page.
- Install USB/IP VHCI driver
  - You can install using `usbip.exe` or manually
  - Using `usbip.exe` install command
    - Run PowerShell or CMD as an Administrator
    - `PS> usbip.exe install`
    - The previous command will install a UDE driver or a WDM driver depending on the available files
      - (UDE version first)
    - `PS> usbip.exe install -u` if UDE driver only
    - `PS> usbip.exe install -w` if WDM driver only
  - Manual Installation for vhci(ude)
    - Run PowerShell or CMD as an Administrator
    - `PS> pnputil /add-driver usbip_vhci_ude.inf`
    - Start Device manager
    - Choose "Add Legacy Hardware" from the "Action" menu.
    - Select "Install the hardware that I manually select from the list".
    - Click "Next".
    - Click "Have Disk", click "Browse", choose the copied folder, and click "OK".
    - Click on the "usbip-win VHCI(ude)", and then click "Next".
    - Click Finish at "Completing the Add/Remove Hardware Wizard".	 
  - Manual Installation for vhci(wdm)
    - Run PowerShell or CMD as an Administrator
    - `PS> pnputil /add-driver usbip_vhci.inf`
    - Start Device manager
    - Choose "Add Legacy Hardware" from the "Action" menu.
    - Select "Install the hardware that I manually select from the list".
    - Click "Next".
    - Click "Have Disk", click "Browse", choose the copied folder, and click "OK".
    - Click on the "USB/IP VHCI Root", and then click "Next".
    - Click Finish at "Completing the Add/Remove Hardware Wizard".
- Attach a remote USB device
  - `PS> usbip.exe attach -r <usbip server ip> -b 2-2`
- Uninstall driver
  - `PS> usbip.exe uninstall`
- Disable test signing
  - `> bcdedit.exe /set TESTSIGNING OFF`
  - reboot the system to apply

### Reporting Bugs
- `usbip-win` is not yet ready for production use. We could find the problems with detailed logs.

#### How to get Windows kernel log for vhci(wdm)
- Set registry key to enable a debug filter
  - `usbip-win` uses [DbgPrintEx API for kernel logging](https://docs.microsoft.com/en-us/windows-hardware/drivers/devtest/reading-and-filtering-debugging-messages).
  - save following as `.reg` file and run or manually insert the registry key
  - reboot the system to apply
```
Windows Registry Editor Version 5.00

[HKEY_LOCAL_MACHINE\SYSTEM\CurrentControlSet\Control\Session Manager\Debug Print Filter]
"IHVDRIVER"=dword:ffffffff
```
- Run a debugging log viewer program before you test.
  - [DebugView](https://docs.microsoft.com/en-us/sysinternals/downloads/debugview) is a good tool to view the logs.
- If your testing machine suffer from BSOD (Blue Screen of Death), you should get it via remote debugging.
  - `WinDbg` on virtual machines would be good to get logs.

#### How to get Windows kernel log for vhci(ude)
- A new vhci(ude) implementation uses WPP SW tracing instead of DbgPrintEx.
  - `DebugView.exe` cannot catch a VHCI debug message
    - `TraceView.exe` is a good utility for a new approach, which is included in WDK.
- `usbip_vhci_ude.pdb` file is required to add a trace provider easily.
- Create a new session log in `TraceView.exe`
  - Choose PDB file radio button in \"*Provider Control GUID Setup*\" popup dialog
  - Specify `usbip_vhci_ude.pdb` as a PDB file
- You can send real-time trace messages to WinDbg by modifying in \"*Advanced Log Session Options*\".
- If your testing machine suffer from BSOD (Blue Screen of Death), you should get it via remote debugging.
  - `WinDbg` on virtual machines would be good to get logs

#### How to get usbip forwarder log
- usbip-win transmits usbip packets via a userland forwarder.
  - forwarder log is the best to look into usbip packet internals. 
- edit `usbip_forward.c` to define `DEBUG_PDU` at the head of the file
- compile `usbip.exe` or `usbipd.exe`
- `debug_pdu.log` is created at the path where an executable runs.  

#### How to get linux kernel log
- Sometimes Linux kernel log is required
```
# dmesg --follow | tee kernel_log.txt
```

<hr>
<sub>This project was supported by Basic Science Research Program through the National Research Foundation of Korea(NRF) funded by the Ministry of Education(2020R1I1A1A01066121).</sub>
