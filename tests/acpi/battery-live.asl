DefinitionBlock ("", "SSDT", 2, "EMBLNK", "BATLIVE", 1)
{
    Scope (\_SB) {
        Device (BAT0) {
            Name (_HID, EisaId ("PNP0C0A"))
            Method (_STA, 0) { Return (0x1F) }
            Method (_BIF, 0) { Return (Package () {
                0, 50000, 40000, 1, 12000, 4000, 2000, 1, 1,
                "Test battery", "123", "LiIon", "Fixture"
            }) }
            Method (_BST, 0) { Return (Package () {1, 10000, 20000, 12000}) }
        }
        Device (AC00) {
            Name (_HID, "ACPI0003")
            Method (_PSR, 0) { Return (1) }
        }
    }
}
