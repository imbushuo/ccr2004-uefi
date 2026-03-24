tftp 192.168.1.10 ros.efi ros.efi
tftp 192.168.1.10 stockfw_initrd.cpio stockfw_initrd.cpio
fs3:\ros.efi initrd=stockfw_initrd.cpio console=ttyS0,115200 benand_no_swecc=2 bootimage=1 yaffs.inband_tags=1 parts=1 arm64=Y board=UEFI_CCR2004-1G-2XS-PCIe ver=2026.324.203 bver=2026.324.203 hw_opt=00100001 boot=1 mlc=12 loglevel=7 earlycon=uart8250,mmio32,0xfd883000,115200n8 earlyprintk verbose
