/* ==========================================================================
 * lex.h -- the shell lexer. Bytes -> tokens. Pure and self-contained: no
 * value system, no allocation of token payloads beyond the token array
 * itself; lexemes are BORROWED slices into the source string (which the
 * caller keeps alive for the token array's lifetime).
 *
 * Locked lexing decisions:
 *   - Both "double" (escapes: \n \t \" \\ \0) and 'single' (raw, no escapes)
 *     quote styles.
 *   - Bare words may contain / . - _ so /foo/bar and file.txt are one IDENT.
 *   - `-` and `.` join a word ONLY when flanked by word chars on both sides:
 *     `sort-by` and `file.txt` are single words; `size - 1` (spaces) is
 *     subtraction; `$row.size` is field access. `size-1` (no spaces) is thus
 *     the single word "size-1" -- a documented gotcha; write spaces for infix.
 *   - filesize literals (1mb, 512kb) are recognized HERE, pre-parsed to bytes.
 *   - `# ` to end of line is a comment.
 *   - keywords (and or not true false let) are their own token types.
 *   - redirections are words with an arrow attached (out> in< ...), never a
 *     bare `>`, which compares.
 * ========================================================================== */
#ifndef __EMBK_LEX_H__
#define __EMBK_LEX_H__

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

enum tok_type {
    TOK_EOF = 0,
    TOK_INT, TOK_FLOAT, TOK_STRING, TOK_FILESIZE,
    TOK_TRUE, TOK_FALSE,
    TOK_IDENT,          /* bare word: command name OR column ref (parser decides) */
    TOK_DOLLAR_IDENT,   /* $name */
    TOK_DOLLAR_LPAREN,  /* $( -- run a pipeline and use its VALUE.
                         * Its own token rather than '$' followed by '(' so
                         * that command substitution is UNAMBIGUOUS. Plain
                         * parentheses group expressions, and `(total)` would
                         * otherwise have to be a guess between "the bare word
                         * total" and "run the command total" -- a guess that
                         * would be wrong silently, in whichever direction it
                         * was made. */
    TOK_LET,            /* let */
    TOK_PIPE,           /* | */
    TOK_LPAREN, TOK_RPAREN,
    TOK_DOT,            /* . as an operator (field access) */
    TOK_COMMA,
    TOK_OR, TOK_AND, TOK_NOT,
    TOK_EQ, TOK_NE, TOK_LT, TOK_GT, TOK_LE, TOK_GE,
    TOK_MATCH,          /* =~ */
    TOK_PLUS, TOK_MINUS, TOK_STAR, TOK_SLASH,
    TOK_ASSIGN,         /* = */

    /* --- statement structure (the shell became a language) ----------------
     * A NEWLINE is a TOKEN, not whitespace. It was whitespace while the shell
     * read exactly one line and handed it to a parser that demanded EOF at the
     * end; the moment a block can span lines, "where does this statement stop"
     * has to be answerable, and the only honest answer in a shell is "at the
     * end of the line, unless the line clearly continues". The parser skips
     * newlines exactly where a statement obviously continues -- after '|',
     * inside (), and around {} -- and treats them as separators everywhere
     * else. That is the rule every shell converges on, and writing it down as
     * a token is what makes it checkable instead of ambient. */
    TOK_NEWLINE,
    TOK_SEMI,           /* ; -- the same separator, spelled inline */
    TOK_AMP,            /* & -- run this statement in the BACKGROUND */
    TOK_LBRACE, TOK_RBRACE,

    TOK_IF, TOK_ELSE, TOK_WHILE, TOK_FOR, TOK_IN,
    TOK_DEF,            /* def name(a, b) { ... } -- a user-defined command */
    TOK_BREAK, TOK_CONTINUE, TOK_RETURN,

    /* --- redirection: where a command's streams go -----------------------
     * `>` is a comparison here (`where size > 1mb`), so a redirection cannot
     * be spelled with it alone. It is a WORD with the arrow attached --
     * out> out>> err> err>> out+err> out+err>> in< -- and only at the start
     * of a word: `out>` written with no space is a redirection, `out > 3`
     * with spaces is still a comparison against a column called out. The
     * token's int_val is the enum redirect_kind below. */
    TOK_REDIRECT,

    TOK_ERROR,          /* malformed input; lexeme = the message */
};

enum redirect_kind {
    REDIR_OUT = 1,          /* out>      stdout to a file, replacing it */
    REDIR_OUT_APPEND,       /* out>>     ... appended                    */
    REDIR_ERR,              /* err>      stderr to a file                */
    REDIR_ERR_APPEND,       /* err>>                                    */
    REDIR_BOTH,             /* out+err>  both to one file                */
    REDIR_BOTH_APPEND,      /* out+err>>                                */
    REDIR_IN,               /* in<       stdin from a file               */
};

struct token {
    enum tok_type type;
    const char   *lexeme;      /* borrowed slice into source (NOT owned).
                                * For TOK_STRING this points at the RAW source
                                * span INCLUDING quotes/escapes; the decoded
                                * string is in `str`/`str_len` below. */
    size_t        lexeme_len;

    /* pre-decoded payloads so the parser never re-scans: */
    int64_t       int_val;     /* TOK_INT, TOK_FILESIZE (bytes) */
    double        float_val;   /* TOK_FLOAT */
    char         *str;         /* TOK_STRING: OWNED decoded bytes (escapes
                                * resolved). NULL for every other type. Freed
                                * by lex_free_tokens(). */
    size_t        str_len;

    size_t        line, col;   /* 1-based, for error messages */
};

/* Tokenize `src` (NUL-terminated) into a freshly-allocated token array
 * terminated by a TOK_EOF token. On a lex error, the array still ends in EOF
 * but contains a TOK_ERROR token (its `lexeme` is a borrowed static message,
 * line/col located) at the point of failure -- the caller checks for it.
 * Returns the array (never NULL on success) and writes the count to *out_n.
 * On allocation failure returns NULL. */
struct token *lex(const char *src, size_t *out_n);

/* Free a token array from lex() -- frees each TOK_STRING's decoded `str`, then
 * the array. */
void lex_free_tokens(struct token *toks, size_t n);

/* Human name for a token type, for error messages / debugging. */
const char *tok_type_name(enum tok_type t);

#endif /* __EMBK_LEX_H__ */