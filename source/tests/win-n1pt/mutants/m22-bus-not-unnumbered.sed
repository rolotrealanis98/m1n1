# file: pcie_t6050.c
s/    write32(pr.rp + PCI_PRIMARY_BUS, read32(pr.rp + PCI_PRIMARY_BUS) \& 0xff0000ff);/    (void)0;/
