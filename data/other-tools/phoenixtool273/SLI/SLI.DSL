Scope (\_SB.PCI0)
{
Name (SLIC, Buffer (0x9E)
{
"987134512781Genuine NVIDIA Certified SLI Ready Motherboard for ASUS RAMPAGE II EXT 3287-Copyright 2008 NVIDIA Corporation All Rights Reserved-765289891023(R)"
})
Device (WMI1)
{
Name (_HID, "pnp0c14")
Name (_UID, "MXM2")
Name (_WDG, Buffer (0x14)
{
/* 0000 */ 0x3C, 0x5C, 0xCB, 0xF6, 0xAE, 0x9C, 0xBD, 0x4E,
/* 0008 */ 0xB5, 0x77, 0x93, 0x1E, 0xA3, 0x2A, 0x2C, 0xC0,
/* 0010 */ 0x4D, 0x58, 0x01, 0x02
})
Method (WMMX, 3, NotSerialized)
{
CreateDWordField (Arg2, 0x00, FUNC)
If (LEqual (FUNC, 0x41494C53))
{
Return (\_SB.PCI0.SLIC)
}

Return (0x00)
}
}
}