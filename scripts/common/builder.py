import subprocess
import sys

class Builder:
    def __init__(self, clean=False):
        self.clean = clean

    def build(self, target="hardware"):
        if self.clean:
            print("Cleaning build directory...")
            subprocess.run(["make", "clean"], check=True)
            
        print("Compiling test_core.elf...")
        subprocess.run(["make", "test_core.elf"], check=True)
        
        # Hardware needs the .bin file extracted to flash via Python bindings
        # QEMU needs qemu_bank1.bin, extflash.bin, sdcard.img
        # make gwemu_media generates all of this
        print("Generating firmware binaries and media...")
        subprocess.run(["make", "gwemu_media"], check=True)
