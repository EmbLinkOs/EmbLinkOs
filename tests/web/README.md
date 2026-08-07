# The web corpus

Pages that exercise the SHAPES real documents turn out to have, kept here so a
regression is caught in two seconds instead of on the next screenshot.

Every page in this directory was written for a bug that actually happened, or
for a feature that is expected to keep working. They are ours, not vendored
copies of other people's sites: the point is the shape, not the content, and a
shape can be reproduced without taking someone's page.

    make web-corpus                 # render every page, fail on any check
    make web-corpus CORPUS=/tmp/x   # ...or a directory of pages you fetched

The second form is how a real site gets used: fetch it somewhere outside the
tree, point the corpus at it, and if it finds something, ADD A PAGE HERE that
reproduces the shape. That is what keeps this file honest — every entry below
names the bug it exists for.

| page | what it pins |
|---|---|
| `deep-inline.html` | links nested nine levels inside `<center>` + nested tables — the depth guard that dropped every Hacker News headline |
| `long-list.html` | a list of 120 items — the 64-child layout cap that stacked items 65+ on the first row's origin |
| `layout-tables.html` | tables used for LAYOUT with `border="0"`, and a data table with `border="1"` |
| `boxes.html` | background, border, radius, padding, text-align, `rgb()` |
| `selectors.html` | the cascade: combinators, specificity, and what must NOT match |
