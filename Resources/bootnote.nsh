tftp 192.168.1.10 ros.efi fs3:\ros.efi
tftp 192.168.1.10 stockfw_initrd.cpio fs3:\stockfw_initrd.cpio
fs3:\ros.efi initrd=fs3:\stockfw_initrd.cpio console=ttyS0,115200 benand_no_swecc=2 bootimage=1 yaffs.inband_tags=1 parts=1 arm64=Y board=CCR2004-1G-2XS-PCIe ver=7.21.3 bver=2026.317.308 hw_opt=00100001 boot=1 mlc=12 loglevel=7 earlycon=uart8250,mmio32,0xfd883000,115200n8 earlyprintk devel
