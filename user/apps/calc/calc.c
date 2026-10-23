/* user/apps/calc/calc.c -- Calculator.
 *
 * A desktop without one is a desktop people leave to do arithmetic somewhere
 * else. That is the whole justification; there is nothing clever here and there
 * should not be.
 *
 * TWO DECISIONS WORTH THE WORDS.
 *
 * It is an IMMEDIATE-EXECUTION calculator, the kind on every desk: each
 * operator finishes the pending one, so 2 + 3 x 4 is 20, not 14. That is not
 * an oversight about precedence -- it is the model the buttons imply. A
 * calculator with keys and no visible expression cannot show you that it is
 * holding 3 x 4 back, so it must not hold anything back.
 *
 * The keyboard works, and it is the same code path as the buttons. A calculator
 * you have to point at is a calculator that is slower than the thing you were
 * already doing. Digits, + - * / , Return or = to finish, Escape or C to clear,
 * Backspace to take back the last digit.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <math.h>

#include "embk.h"
#include "ui.h"
#include "em.h"
#include "theme.h"

/* The display holds TEXT, not a number, for as long as it is being typed:
 * "0.10" and "0.1" are the same value and a person typing the first should not
 * watch it become the second under their fingers. It becomes a double only
 * when an operator or = says the entry is finished. */
static char   g_entry[24] = "0";
static bool   g_typing;            /* the display is a number being entered */
static double g_acc;               /* the value on the left of g_op */
static char   g_op;                /* 0 = none, else + - * / */
static bool   g_error;             /* division by zero: shown, not hidden */

static void show_number(double v) {
    if (v != v || v > 1e15 || v < -1e15) {          /* NaN or out of a readable range */
        snprintf(g_entry, sizeof g_entry, "Error");
        g_error = true;
        return;
    }
    /* Integers print as integers. "12" reads as twelve; "12.000000" reads as a
     * machine that does not know what twelve is. */
    if (v == (double)(long long)v && v < 1e15 && v > -1e15)
        snprintf(g_entry, sizeof g_entry, "%lld", (long long)v);
    else
        snprintf(g_entry, sizeof g_entry, "%.10g", v);
}

static double entry_value(void) { return atof(g_entry); }

static void clear_all(void) {
    snprintf(g_entry, sizeof g_entry, "0");
    g_typing = false; g_acc = 0; g_op = 0; g_error = false;
}

static void digit(char c) {
    if (g_error) clear_all();
    if (!g_typing) { g_entry[0] = 0; g_typing = true; }
    int n = (int)strlen(g_entry);
    if (n >= (int)sizeof g_entry - 1) return;        /* full: ignore, never wrap */
    if (c == '.') {
        if (strchr(g_entry, '.')) return;            /* one point per number */
        if (!n) { g_entry[n++] = '0'; g_entry[n] = 0; }
    }
    if (!n && c == '0') { snprintf(g_entry, sizeof g_entry, "0"); return; }
    g_entry[n] = c; g_entry[n + 1] = 0;
    if (!g_entry[0]) snprintf(g_entry, sizeof g_entry, "0");
}

static void apply_pending(void) {
    double rhs = entry_value();
    switch (g_op) {
        case '+': g_acc += rhs; break;
        case '-': g_acc -= rhs; break;
        case '*': g_acc *= rhs; break;
        case '/':
            /* DIVISION BY ZERO IS SHOWN. Silently leaving the previous result
             * on screen would look like the key did nothing, and the one thing
             * a calculator must never do is appear to have accepted input it
             * discarded. */
            if (rhs == 0.0) { snprintf(g_entry, sizeof g_entry, "Error");
                              g_error = true; g_op = 0; g_typing = false; return; }
            g_acc /= rhs; break;
        default:  g_acc = rhs; break;
    }
    show_number(g_acc);
}

static void operator_key(char op) {
    if (g_error) return;
    apply_pending();
    if (g_error) return;
    g_op = op;
    g_typing = false;             /* the next digit starts a fresh number */
}

static void equals_key(void) {
    if (g_error) return;
    apply_pending();
    g_op = 0;
    g_typing = false;
    /* SAY THE ANSWER ON THE LOG. A GUI app cannot report on itself -- its
     * output is pixels -- so the only way to test that this calculator
     * calculates, rather than merely that its buttons light up, is for the
     * result to leave the process as text. One line, only on equals. */
    { char b[48]; snprintf(b, sizeof b, "calc: = %s\n", g_entry); embk_puts(1, b); }
}

static void backspace_key(void) {
    if (g_error) { clear_all(); return; }
    if (!g_typing) return;                  /* nothing being typed to take back */
    int n = (int)strlen(g_entry);
    if (n > 1) g_entry[n - 1] = 0;
    else { snprintf(g_entry, sizeof g_entry, "0"); g_typing = false; }
}

static void sign_key(void) {
    if (g_error) return;
    if (g_entry[0] == '-') memmove(g_entry, g_entry + 1, strlen(g_entry));
    else if (strcmp(g_entry, "0") != 0) {
        char t[sizeof g_entry];
        snprintf(t, sizeof t, "-%s", g_entry);
        snprintf(g_entry, sizeof g_entry, "%s", t);
    }
}

static void percent_key(void) {
    if (g_error) return;
    show_number(entry_value() / 100.0);
    g_typing = false;
}

/* One key. Every button routes here and so does the keyboard, so there is
 * exactly one description of what each key means. */
static void press(char k) {
    if (k >= '0' && k <= '9') { digit(k); return; }
    switch (k) {
        case '.': digit('.'); break;
        case '+': case '-': case '*': case '/': operator_key(k); break;
        case '=': equals_key(); break;
        case 'C': clear_all(); break;
        case 'B': backspace_key(); break;
        case 'S': sign_key(); break;
        case '%': percent_key(); break;
        default: break;
    }
}

/* A key on the pad. `tone` picks the three roles a calculator has: digits,
 * operators, and the row that changes what the machine is holding. */
static void key(const char *label, char k, int tone) {
    const struct ui_theme *t = ui_theme();
    Color bg = tone == 2 ? t->accent
             : tone == 1 ? t->surface_alt
             : t->surface;
    Color fg = tone == 2 ? t->on_accent : t->text;
    bool hit = false;
    HStack(.grow = 1, .height = 52, .corner = 10, .background = bg,
           .align = Center, .justify = Center) {
        struct instance_handle self = ui_open();
        Text(label).color(fg).font(Subtitle);
        em_flush();
        hit = ui_consume_click(self);
    }
    if (hit) press(k);
}

/* THE KEYBOARD IS THE SAME CODE PATH AS THE BUTTONS -- one description of what
 * each key means, so the pad and the keys can never disagree about what "/" or
 * Escape does. A calculator you have to point at is slower than the thing you
 * were already doing. */
static int on_key(int ch) {
    if (ch >= '0' && ch <= '9') { press((char)ch); return 1; }
    switch (ch) {
        case '.': case ',':                      press('.'); return 1;
        case '+': case '-': case '*': case '/':  press((char)ch); return 1;
        case '=': case '\n': case '\r':          press('='); return 1;
        case 8: case 127:                        press('B'); return 1;
        case 27: case 'c': case 'C':             press('C'); return 1;
        case '%':                                press('%'); return 1;
        default: return 0;                       /* not ours -- let it pass */
    }
}

static void app(void) {
    const struct ui_theme *t = ui_theme();
    static int hooked = 0;
    if (!hooked) { hooked = 1; em_set_key_hook(on_key); }

    Window("Calculator") {
        AppBar("Calculator") { }
        VStack(.padding = 14, .spacing = 10, .grow = 1, .align = Fill) {
            /* The display. Trailing, large, and the only thing here that is
             * allowed to be big -- it is the answer, and everything else is a
             * way of asking. */
            HStack(.height = 64, .corner = 12, .background = t->surface_alt,
                   .px = 14, .align = Center) {
                Spacer();
                Text(g_entry).font(Heading)
                             .color(g_error ? t->danger : t->text);
            }

            /* An operator is PENDING, and saying so is the difference between
             * a calculator you can trust mid-sum and one you have to restart to
             * be sure of. */
            HStack(.height = 16, .px = 4, .align = Center) {
                if (g_op) {
                    char w[40];
                    snprintf(w, sizeof w, "%c pending", g_op);
                    Text(w).caption().tertiary();
                }
                Spacer();
            }

            VStack(.spacing = 8, .grow = 1, .align = Fill) {
                HStack(.spacing = 8, .grow = 1) {
                    key("C", 'C', 1); key("+/-", 'S', 1);
                    key("%", '%', 1); key("/", '/', 2);
                }
                HStack(.spacing = 8, .grow = 1) {
                    key("7", '7', 0); key("8", '8', 0);
                    key("9", '9', 0); key("x", '*', 2);
                }
                HStack(.spacing = 8, .grow = 1) {
                    key("4", '4', 0); key("5", '5', 0);
                    key("6", '6', 0); key("-", '-', 2);
                }
                HStack(.spacing = 8, .grow = 1) {
                    key("1", '1', 0); key("2", '2', 0);
                    key("3", '3', 0); key("+", '+', 2);
                }
                HStack(.spacing = 8, .grow = 1) {
                    key("0", '0', 0); key(".", '.', 0);
                    key("<", 'B', 0); key("=", '=', 2);
                }
            }
        }
    }
}

EM_APPLICATION {
    .title  = "Calculator",
    .size   = { 300, 460 },
    .chrome = Chromeless,
    .view   = app,
};
