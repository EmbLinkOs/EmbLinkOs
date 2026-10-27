# Inter

The interface's face: Inter, by Rasmus Andersson -- a face drawn for screens,
with tall lowercase and open apertures that stay readable at the sizes an
interface uses, and the closest open face to the system faces of current
desktops. Regular is the UI's text face (`/system/fonts/font.ttf`) and
SemiBold its emphasis (`bold.ttf`): a heavy Bold reads as shouting at UI
sizes. DejaVu Sans stays on the image as the fallback for what Inter does not
cover (`fallback.ttf`, ui/backend/font.h).

| File | SHA-256 |
| --- | --- |
| Inter-Regular.ttf | 40d692fce188e4471e2b3cba937be967878f631ad3ebbbdcd587687c7ebe0c82 |
| Inter-SemiBold.ttf | 78a843fade9d4612a5567302fb595b56976eb5fcebf4fea5a5912d638bafcde3 |

Source: the Inter v4.1 release archive `Inter-4.1.zip`
(https://github.com/rsms/inter/releases/download/v4.1/Inter-4.1.zip),
`extras/ttf/`, unmodified -- the static TrueType (glyf) builds, which the
rasteriser reads; the variable build needs gvar, which it does not.
`LICENSE.txt` is the archive's own.

Licence: SIL Open Font License 1.1 (`LICENSE.txt`, and
`system/licenses/inter-fonts.txt` on the image).
