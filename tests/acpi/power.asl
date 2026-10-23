// Synthetic firmware fixture: exercises AML evaluation, not battery hardware.
DefinitionBlock ("", "SSDT", 2, "EMBLNK", "PWRTEST", 1)
{
    Scope (\_SB)
    {
        Device (BAT0)
        {
            Name (_HID, EisaId ("PNP0C0A"))
            Method (_STA, 0) { Return (0x1F) }
            Method (_BIF, 0) { Return (Package () {
                0, 50000, 40000, 1, 12000, 4000, 2000, 1, 1,
                "Test battery", "123", "LiIon", "Fixture"
            }) }
            Method (_BST, 0) { Return (Package () {1, 10000, 20000, 12000}) }
        }
        Device (BAT1)
        {
            Name (_HID, EisaId ("PNP0C0A"))
            Method (_STA, 0) { Return (0x1F) }
            Method (_BIX, 0) { Return (Package () {
                0, 1, 6000, 5000, 1, 12000, 500, 200, 20, 1000,
                1, 1, 1, 1, 1, 1, "Test BIX", "456", "LiIon", "Fixture"
            }) }
            Method (_BST, 0) { Return (Package () {2, 1000, 1250, 12500}) }
        }
        Device (BAT2)
        {
            Name (_HID, EisaId ("PNP0C0A"))
            Method (_STA, 0) { Return (0x1F) }
            Method (_BST, 0) { Return (Package () {0, 0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF}) }
        }
        Device (AC00)
        {
            Name (_HID, "ACPI0003")
            Method (_PSR, 0) { Return (1) }
        }
        Device (LID0)
        {
            Name (_HID, EisaId ("PNP0C0D"))
            Method (_LID, 0) { Return (1) }
        }
        Device (VID0)
        {
            Name (_HID, "EMBL0001")
            Device (LCD0)
            {
                Name (_ADR, 0x0400)
                Name (BRT0, 60)
                Method (_BCL, 0) { Return (Package () { 80, 60, 100, 0, 20, 40, 60, 80, 40 }) }
                Method (_BQC, 0) { Return (BRT0) }
                Method (_BCM, 1) { Store (Arg0, BRT0) }
            }
        }
    }
}
