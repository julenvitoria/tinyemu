# Alleycat Native HLE Implementation Plan (Finalized)

## Goal Description
We will execute Alleycat (1984) natively on the STM32 by bypassing traditional x86 DOS emulation. Our C-based host code will mount the `ALLEYCAT.dsk` FAT12 floppy image, load the executable into guest memory, build the PSP, and our Thumb2 dispatcher will intercept basic BIOS/DOS interrupts (`INT 10h`, `16h`, `1Ah`, `21h`) to provide Native HLE. 

## Strategy: Mounting the `.dsk` Image
As requested, we will retain the `.dsk` format and mount it using the FatFs driver. 
Since `retro-go` already includes FatFs (used for reading the SD card), we will create a **virtual `diskio` driver** in our emulator. This virtual driver will expose the contents of the `ALLEYCAT.dsk` file (read from the physical SD card) to a secondary logical FatFs drive (e.g., `1:`). 
This enables us to use standard `f_open("1:ALLEYCAT.EXE")` calls within our loader to directly extract the game from the disk image without requiring custom FAT12 parsing code!

## Proposed Changes

### Phase 1: Virtual FatFs Drive
- **[NEW] `vdisk.c` / `vdisk.h`**: Implement a FatFs `disk_read` / `disk_write` backend that maps logical sector reads to file offset reads inside an open `.dsk` file on the primary SD card.
- **[MODIFY] `tests/main.c`**: Initialize FatFs, mount the primary SD card (drive `0:`), open `ALLEYCAT.dsk`, and then mount our virtual drive (drive `1:`).

### Phase 2: MZ Executable Loader
- **[NEW] `loader.c`**: Implement a function to open `1:ALLEYCAT.EXE`, read its MZ header, and allocate it into `test_ram`.
- Construct the 256-byte Program Segment Prefix (PSP) at the appropriate segment.
- Initialize `cpu_state_dtcm` registers (`CS`, `IP`, `SS`, `SP`) based on the header.

### Phase 3: Native HLE Dispatcher (Thumb2)
- **[MODIFY] `cpu_exec.S`**: Hook the `INT n` (0xCD) opcode. If `n` is `0x10`, `0x16`, `0x1A`, or `0x21`, we exit the x86 loop (`EXIT_HLE_INT`).

### Phase 4: Native HLE Handlers
- **[NEW] `hle.c`**: 
  - `INT 10h`: Map CGA framebuffer (`0xB8000`), handle mode sets (`AH=00h`).
  - `INT 16h`: Basic keyboard stubs.
  - `INT 1Ah`: Timer stubs.
  - `INT 21h`: Terminate program (`AH=4Ch`).
- **[MODIFY] `tests/main.c`**: Add the CGA rendering loop to draw the `0xB8000` memory to the screen.

---

## Verification Plan
1. **Virtual Disk Test**: Print the root directory of `1:` to the UART to verify the `.dsk` image is mounted successfully via FatFs.
2. **Execution Test**: Verify the game starts executing and hits our `INT 10h` HLE handler to set the video mode to CGA.
3. **Rendering Test**: Validate the CGA screen displays correctly on the STM32.
