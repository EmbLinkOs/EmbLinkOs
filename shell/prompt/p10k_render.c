/* ==========================================================================
 * p10k_render.c -- Powerlevel10k's prompt, drawn: the segments, the glue
 * between them, the lines with their frames and the gap, the directory
 * shortened to fit, the transient prompt.
 *
 * Each piece follows the zsh function it is named after in Powerlevel10k's
 * internal/p10k.zsh (1.20.0): _p9k_left_prompt_segment, _p9k_right_prompt_
 * segment, _p9k_init_lines, _p9k_build_gap_post, _p9k_set_prompt, and the
 * prompt_* segments. They build zsh prompt strings the way p10k does; the
 * last step expands them to SGR (p10k_zsh.c).
 * ========================================================================== */
#include "prompt/p10k_int.h"
#include "edit/wcwidth.h"
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* --- one side of one line: p10k's _p9k__bg, _p9k__s, _p9k__ss, _p9k__sss, _p9k__w */
struct side {
    bool none;                   /* nothing drawn yet: _p9k__bg == NONE */
    char bg[16];                 /* the last drawn segment's background */
    int last;                    /* its index (_p9k__i) */
    bool emitted;                /* a segment put code in the prompt, shown or not:
                                  * a right side then exists (and a line's gap) */
    struct sb s, ss, sss, w;
};

/* The directory as prompt_dir builds it (seg_dir): each part's text, and for
 * truncate_to_unique the shorter form of those it may shorten. The line's
 * budget (p10k's _p9k__d, in p10k_render) chooses how many, left to right. */
#define DIR_PARTS 128
struct dirpart { struct sb full, alt; int saved; bool shortenable; };
struct dirmodel {
    bool expand;                 /* truncate_to_unique: shortened as the line needs */
    int n;
    struct dirpart part[DIR_PARTS];
    struct sb sep, header, footer;
    int width;                   /* the content with every part full (_p9k__dir_len) */
    int upto;                    /* parts [0, upto) drawn short */
};

struct ctx {
    const struct p10k *cfg;
    const struct p10k_state *st;
    int cols;
    bool right;                  /* building a right side */
    struct sb *out;
    struct side *side;
    int index;                   /* the segment's index on its side */
    int join_from;               /* a _joined segment joins anything drawn since this index */
    bool joined;
    struct dirmodel *dir;        /* the left side of a line: its directory's budget */
    bool has_dir;                /* a directory took the budget on this line */
    bool dir_before;             /* one did on an earlier line: _p9k__d=-1024 for the rest */
};

static const char *COLOR1 = "0", *COLOR2 = "7";     /* COLOR_SCHEME dark */

static void fg(struct sb *b, const char *c) { if (c[0]) sb_fmt(b, "%%F{%s}", c); else sb_str(b, "%f"); }
static void bg(struct sb *b, const char *c) { if (c[0]) sb_fmt(b, "%%K{%s}", c); else sb_str(b, "%k"); }
static bool has_pct(const char *s) { return strchr(s, '%') != NULL; }

/* Text as content: % doubled, so zsh shows it as written. */
static void esc(struct sb *b, const char *s) { for (; *s; s++) { if (*s == '%') sb_chr(b, '%'); sb_chr(b, *s); } }

/* --- a segment ---------------------------------------------------------------- *
 * _p9k_left_prompt_segment / _p9k_right_prompt_segment. `name` is its state
 * ("prompt_dir_HOME"), `bgd` and `fgd` its default colours, `icon_name` an
 * icon to look up (or "\001literal", or ""), `content` a zsh prompt string. */
static void segment(struct ctx *c, const char *name, const char *bgd, const char *fgd, const char *icon_name,
                    const char *content) {
    const struct p10k *cfg = c->cfg;
    const char *L = c->right ? "RIGHT" : "LEFT";
    char bgc[16], fgc[16], key[64];
    c->side->emitted = true;
    p9k_color(cfg, name, "BACKGROUND", bgd, bgc);
    p9k_color(cfg, name, "FOREGROUND", fgd, fgc);
    struct sb style = { 0 }, sep = { 0 }, subsep = { 0 }, icon = { 0 }, start = { 0 }, endsep = { 0 };
    struct sb space = { 0 }, lspace = { 0 }, rspace = { 0 }, v = { 0 }, cc = { 0 }, tmp = { 0 }, vstyle = { 0 };
    sb_str(&style, "%b"); bg(&style, bgc); fg(&style, fgc);
    snprintf(key, sizeof key, "%s_SEGMENT_SEPARATOR", L);
    p9k_icon(cfg, name, key, NULL, &sep);
    snprintf(key, sizeof key, "%s_SUBSEGMENT_SEPARATOR", L);
    p9k_icon(cfg, name, key, NULL, &subsep);
    if (c->right && has_pct(sb_text(&subsep))) sb_str(&subsep, sb_text(&style));
    if (icon_name && icon_name[0]) p9k_icon(cfg, name, icon_name, NULL, &icon);
    if (c->right) {
        p9k_icon(cfg, name, "RIGHT_PROMPT_FIRST_SEGMENT_START_SYMBOL", sb_text(&sep), &tmp);
        if (sb_text(&tmp)[0]) { sb_str(&start, "%b%k%F{"); sb_str(&start, bgc); sb_str(&start, "}"); sb_str(&start, sb_text(&tmp)); }
        p9k_icon(cfg, name, "RIGHT_PROMPT_LAST_SEGMENT_END_SYMBOL", NULL, &endsep);
    } else {
        p9k_icon(cfg, name, "LEFT_PROMPT_FIRST_SEGMENT_START_SYMBOL", NULL, &tmp);
        if (sb_text(&tmp)[0]) { sb_str(&start, "%b%k%F{"); sb_str(&start, bgc); sb_str(&start, "}"); sb_str(&start, sb_text(&tmp)); }
        p9k_icon(cfg, name, "LEFT_PROMPT_LAST_SEGMENT_END_SYMBOL", sb_text(&sep), &endsep);
    }
    snprintf(key, sizeof key, "WHITESPACE_BETWEEN_%s_SEGMENTS", L);
    p9k_icon(cfg, name, key, " ", &space);
    snprintf(key, sizeof key, "%s_LEFT_WHITESPACE", L);
    p9k_icon(cfg, name, key, sb_text(&space), &lspace);
    if (has_pct(sb_text(&lspace))) sb_str(&lspace, sb_text(&style));
    snprintf(key, sizeof key, "%s_RIGHT_WHITESPACE", L);
    p9k_icon(cfg, name, key, sb_text(&space), &rspace);
    if (has_pct(sb_text(&rspace))) sb_str(&rspace, sb_text(&style));

    /* the icon and the content, through their expansions */
    const char *iexp = p9k_param(cfg, name, "VISUAL_IDENTIFIER_EXPANSION", "${P9K_VISUAL_IDENTIFIER}");
    const char *cexp = p9k_param(cfg, name, "CONTENT_EXPANSION", "${P9K_CONTENT}");
    int has_icon;
    if (strcmp(iexp, "${P9K_VISUAL_IDENTIFIER}") && strchr(iexp, '$')) {
        p9k_expand_param(cfg, iexp, content, sb_text(&icon), c->st, &v);
        if (sb_text(&v)[0]) sb_str(&v, sb_text(&style));
        has_icon = -1;
    } else {
        p9k_decode_escapes(!strcmp(iexp, "${P9K_VISUAL_IDENTIFIER}") ? sb_text(&icon) : iexp, &v);
        has_icon = sb_text(&v)[0] ? 1 : 0;
        if (has_icon && has_pct(sb_text(&v))) sb_str(&v, sb_text(&style));
    }
    p9k_expand_param(cfg, cexp, content, sb_text(&icon), c->st, &cc);
    { char *p = cc.p, *q = cc.p; if (p) { for (; *p; p++) if (*p != '\r') *q++ = *p; *q = 0; cc.n = (size_t)(q - cc.p); } }
    bool c_vis = p10k_width(sb_text(&cc), c->st) > 0;
    bool v_vis = has_icon == -1 ? p10k_width(sb_text(&v), c->st) > 0 : has_icon == 1;
    if (!c_vis && !v_vis) goto done;                 /* nothing to show: not drawn */

    /* the glue: what goes before it, by what was before it */
    struct side *sd = c->side;
    struct sb *out = c->out;
    bool joined = c->joined && !sd->none && sd->last >= c->join_from;
    bool same_bg = !sd->none && !strcmp(bgc, sd->bg);
    if (!c->right) {
        if (sd->none) { sb_str(out, sb_text(&start)); sb_str(out, sb_text(&style)); sb_str(out, sb_text(&lspace)); }
        else if (joined) sb_str(out, sb_text(&style));
        else if (same_bg) {
            sb_str(out, "%b"); bg(out, bgc);
            if (fgc[0] && !strcmp(fgc, bgc)) { char x[16]; p9k_translate_color(!strcmp(fgc, "000") ? COLOR2 : COLOR1, x); fg(out, x); }
            sb_str(out, sb_text(&sd->ss)); sb_str(out, sb_text(&style)); sb_str(out, sb_text(&lspace));
        } else {
            sb_str(out, "%b"); bg(out, bgc); sb_str(out, sb_text(&sd->s)); sb_str(out, sb_text(&style)); sb_str(out, sb_text(&lspace));
        }
    } else {
        if (sd->none) { sb_str(out, sb_text(&start)); sb_str(out, sb_text(&style)); sb_str(out, sb_text(&lspace)); }
        else if (joined) { sb_str(out, sb_text(&sd->w)); sb_str(out, sb_text(&style)); }
        else if (same_bg) { sb_str(out, sb_text(&sd->w)); sb_str(out, sb_text(&style)); sb_str(out, sb_text(&subsep)); sb_str(out, sb_text(&lspace)); }
        else {
            sb_str(out, sb_text(&sd->w)); sb_str(out, "%F{"); sb_str(out, bgc); sb_str(out, "}");
            sb_str(out, sb_text(&sep)); sb_str(out, sb_text(&style)); sb_str(out, sb_text(&lspace));
        }
    }

    /* the body: the icon before or after the content */
    const char *ibc = p9k_param(cfg, name, "ICON_BEFORE_CONTENT", "");
    bool icon_first = c->right ? !strcmp(ibc, "true") : strcmp(ibc, "false") != 0;
    struct sb prefix = { 0 }, suffix = { 0 }, mws = { 0 };
    p9k_decode_escapes(p9k_param(cfg, name, "PREFIX", ""), &prefix);
    snprintf(key, sizeof key, "%s_MIDDLE_WHITESPACE", L);
    p9k_icon(cfg, name, key, " ", &mws);
    char vc[16];
    p9k_color(cfg, name, "VISUAL_IDENTIFIER_COLOR", fgc, vc);
    sb_str(&vstyle, "%b"); bg(&vstyle, bgc); fg(&vstyle, vc);
    bool need = has_pct(sb_text(&prefix));
    sb_str(out, sb_text(&prefix));
    if (icon_first) {
        if (v_vis) {
            if (strcmp(sb_text(&vstyle), sb_text(&style))) { sb_str(out, sb_text(&vstyle)); sb_str(out, sb_text(&v)); sb_str(out, sb_text(&style)); }
            else { if (need) sb_str(out, sb_text(&style)); sb_str(out, sb_text(&v)); }
            if (c_vis && sb_text(&mws)[0]) { sb_str(out, sb_text(&mws)); if (has_pct(sb_text(&mws))) sb_str(out, sb_text(&style)); }
        } else if (need) sb_str(out, sb_text(&style));
        sb_str(out, sb_text(&cc)); sb_str(out, sb_text(&style));
    } else {
        if (need) sb_str(out, sb_text(&style));
        sb_str(out, sb_text(&cc)); sb_str(out, sb_text(&style));
        if (v_vis) {
            bool need2 = false;
            if (c_vis && sb_text(&mws)[0]) { sb_str(out, sb_text(&mws)); need2 = has_pct(sb_text(&mws)); }
            if (strcmp(sb_text(&vstyle), sb_text(&style)) || need2) sb_str(out, sb_text(&vstyle));
            sb_str(out, sb_text(&v));
        }
    }
    p9k_decode_escapes(p9k_param(cfg, name, "SUFFIX", ""), &suffix);
    sb_str(out, sb_text(&suffix));
    if (!c->right) {
        if (has_pct(sb_text(&suffix)) && sb_text(&rspace)[0]) sb_str(out, sb_text(&style));
        sb_str(out, sb_text(&rspace));
    }

    /* what the next segment needs to know */
    sb_reset(&sd->s); sb_str(&sd->s, "%F{"); sb_str(&sd->s, bgc); sb_str(&sd->s, "}"); sb_str(&sd->s, sb_text(&sep));
    sb_reset(&sd->ss); sb_str(&sd->ss, sb_text(&subsep));
    sb_reset(&sd->sss);
    if (!c->right) { sb_str(&sd->sss, "%F{"); sb_str(&sd->sss, bgc); sb_str(&sd->sss, "}"); sb_str(&sd->sss, sb_text(&endsep)); }
    else {
        sb_reset(&sd->w);
        if (sb_text(&rspace)[0]) sb_str(&sd->w, sb_text(&style));
        sb_str(&sd->w, sb_text(&rspace)); sb_str(&sd->w, "%b"); bg(&sd->w, bgc);
        if (fgc[0] && !strcmp(fgc, bgc)) { char x[16]; p9k_translate_color(!strcmp(fgc, "000") ? COLOR2 : COLOR1, x); fg(&sd->w, x); }
        else fg(&sd->w, fgc);
        sb_str(&sd->sss, sb_text(&style)); sb_str(&sd->sss, sb_text(&rspace));
        if (has_pct(sb_text(&rspace))) sb_str(&sd->sss, sb_text(&style));
        if (sb_text(&endsep)[0]) {
            sb_str(&sd->sss, "%k%F{"); sb_str(&sd->sss, bgc); sb_str(&sd->sss, "}"); sb_str(&sd->sss, sb_text(&endsep));
            sb_str(&sd->sss, sb_text(&style));
        }
    }
    snprintf(sd->bg, sizeof sd->bg, "%s", bgc);
    sd->none = false;
    sd->last = c->index;
    sb_free(&prefix); sb_free(&suffix); sb_free(&mws);
done:
    sb_free(&style); sb_free(&sep); sb_free(&subsep); sb_free(&icon); sb_free(&start); sb_free(&endsep);
    sb_free(&space); sb_free(&lspace); sb_free(&rspace); sb_free(&v); sb_free(&cc); sb_free(&tmp); sb_free(&vstyle);
}

/* --- segments ------------------------------------------------------------------ */

static const char *VCS_STATE_BG[] = { "2", "3", "2", "8", "3" };   /* CLEAN MODIFIED UNTRACKED LOADING CONFLICTED */
static const char *VCS_STATE[] = { "CLEAN", "MODIFIED", "UNTRACKED", "LOADING", "CONFLICTED" };

/* VCS_GIT_REMOTE_ICONS' defaults: the forge's icon by the remote's host. */
static const char *vcs_icon(const struct p10k_git *g) {
    static const struct { const char *domain, *icon; } forge[] = {
        { "archlinux.org", "VCS_GIT_ARCHLINUX_ICON" }, { "dev.azure.com", "VCS_GIT_AZURE_ICON" },
        { "visualstudio.com", "VCS_GIT_AZURE_ICON" }, { "bitbucket.org", "VCS_GIT_BITBUCKET_ICON" },
        { "codeberg.org", "VCS_GIT_CODEBERG_ICON" }, { "debian.org", "VCS_GIT_DEBIAN_ICON" },
        { "freebsd.org", "VCS_GIT_FREEBSD_ICON" }, { "freedesktop.org", "VCS_GIT_FREEDESKTOP_ICON" },
        { "gitea.com", "VCS_GIT_GITEA_ICON" }, { "gitea.io", "VCS_GIT_GITEA_ICON" },
        { "github.com", "VCS_GIT_GITHUB_ICON" }, { "gitlab.com", "VCS_GIT_GITLAB_ICON" },
        { "gnome.org", "VCS_GIT_GNOME_ICON" }, { "gnu.org", "VCS_GIT_GNU_ICON" }, { "kde.org", "VCS_GIT_KDE_ICON" },
        { "kernel.org", "VCS_GIT_LINUX_ICON" }, { "sourcehut.org", "VCS_GIT_SOURCEHUT_ICON" },
    };
    for (size_t i = 0; i < sizeof forge / sizeof forge[0]; i++) {
        const char *at = strstr(g->remote_url, forge[i].domain);
        if (!at) continue;
        char before = at == g->remote_url ? '/' : at[-1], after = at[strlen(forge[i].domain)];
        if ((before == '/' || before == '.' || before == '@') && (!after || after == '/' || after == ':' || after == '?' || after == '#'))
            return forge[i].icon;
    }
    return "VCS_GIT_ICON";
}

static bool hook(const struct p10k *cfg, const char *name) {
    const char *h = p10k_get(cfg, "VCS_GIT_HOOKS");
    if (!h) return true;                      /* the default list has all of them */
    char e[48];
    size_t n = p9k_array(h, 0, e, sizeof e);
    for (size_t i = 0; i < n; i++) { p9k_array(h, i, e, sizeof e); if (!strcmp(e, name)) return true; }
    return false;
}

/* _p9k_vcs_style: a part's colours in the state */
static void vcs_style(const struct p10k *cfg, const char *state, const char *part, struct sb *out) {
    char seg[48], c[16], var[96];
    int si = 0;
    for (int i = 0; i < 5; i++) if (!strcmp(VCS_STATE[i], state)) si = i;
    snprintf(seg, sizeof seg, "prompt_vcs_%s", state);
    sb_str(out, "%b");
    p9k_color(cfg, seg, "BACKGROUND", VCS_STATE_BG[si], c);
    bg(out, c);
    snprintf(var, sizeof var, "VCS_%s_%sFORMAT_FOREGROUND", state, part);
    const char *v = p10k_get(cfg, var);
    if (!v) { snprintf(var, sizeof var, "VCS_%sFORMAT_FOREGROUND", part); v = p10k_get(cfg, var); }
    if (!v && !strcmp(part, "ACTION")) v = "1";                    /* VCS_ACTIONFORMAT_FOREGROUND's default */
    if (v) p9k_translate_color(v, c); else p9k_color(cfg, seg, "FOREGROUND", COLOR1, c);
    fg(out, c);
}

static void seg_vcs(struct ctx *c) {
    const struct p10k *cfg = c->cfg;
    const struct p10k_git *g = &c->st->git;
    c->side->emitted = true;                          /* ${(e)_p9k__vcs}, filled or not */
    if (!g->repo) return;
    char name[48];
    /* _p9k_vcs_render. Loading: what is known so far, in LOADING's colours;
     * nothing yet: VCS_LOADING_TEXT. */
    const char *state = NULL;
    if (g->loading) {
        if (!g->branch[0] && !g->commit[0] && !g->tag[0]) {
            struct sb t = { 0 };
            p9k_decode_escapes(p9k_param(cfg, "", "VCS_LOADING_TEXT", "loading"), &t);
            segment(c, "prompt_vcs_LOADING", "8", COLOR1, "VCS_LOADING_ICON", sb_text(&t));
            sb_free(&t);
            return;
        }
        state = "LOADING";
    }
    bool conflicted = g->conflicted && p9k_bool(cfg, "VCS_CONFLICTED_STATE", false);
    int si = 0;
    if (p9k_bool(cfg, "VCS_DISABLE_GITSTATUS_FORMATTING", false)) {
        if (!state) state = conflicted ? "CONFLICTED" : g->staged || g->unstaged ? "MODIFIED" : g->untracked ? "UNTRACKED" : "CLEAN";
        for (int i = 0; i < 5; i++) if (!strcmp(VCS_STATE[i], state)) si = i;
        snprintf(name, sizeof name, "prompt_vcs_%s", state);
        segment(c, name, VCS_STATE_BG[si], COLOR1, vcs_icon(g), "");
        return;
    }
    /* _p9k_vcs_render: Powerlevel10k's own formatting */
    int untracked = hook(cfg, "git-untracked") ? g->untracked : 0;
    int ahead = hook(cfg, "git-aheadbehind") ? g->ahead : 0, behind = hook(cfg, "git-aheadbehind") ? g->behind : 0;
    int stashes = hook(cfg, "git-stash") ? g->stashes : 0;
    const char *remote = hook(cfg, "git-remotebranch") ? g->remote_branch : "";
    const char *tag = hook(cfg, "git-tagname") ? g->tag : "";
    long amax = p9k_int(cfg, "VCS_COMMITS_AHEAD_MAX_NUM", -1), bmax = p9k_int(cfg, "VCS_COMMITS_BEHIND_MAX_NUM", -1);
    if (amax >= 0 && ahead > amax) ahead = (int)amax;
    if (bmax >= 0 && behind > bmax) behind = (int)bmax;
    const char *icon = "";
    if (hook(cfg, "vcs-detect-changes")) {
        if (state) {}
        else if (conflicted) state = "CONFLICTED";
        else if (g->staged || g->unstaged) state = "MODIFIED";
        else if (untracked) state = "UNTRACKED";
        icon = vcs_icon(g);
    }
    if (!state) state = "CLEAN";
    for (int i = 0; i < 5; i++) if (!strcmp(VCS_STATE[i], state)) si = i;
    snprintf(name, sizeof name, "prompt_vcs_%s", state);
    struct sb content = { 0 }, ic = { 0 };
    const char *ws = "";
    bool changeset = p9k_bool(cfg, "SHOW_CHANGESET", false);
    if (changeset || !g->branch[0]) {
        vcs_style(cfg, state, "COMMIT", &content);
        sb_str(&content, p9k_icon(cfg, name, "VCS_COMMIT_ICON", NULL, &ic));
        long len = p9k_int(cfg, "CHANGESET_HASH_LENGTH", 8);
        if (g->commit[0]) sb_add(&content, g->commit, strlen(g->commit) < (size_t)len ? strlen(g->commit) : (size_t)len);
        else sb_str(&content, "HEAD");
        ws = " ";
    }
    if (g->branch[0]) {
        vcs_style(cfg, state, "BRANCH", &content);
        sb_str(&content, ws);
        if (!p9k_bool(cfg, "HIDE_BRANCH_ICON", false)) sb_str(&content, p9k_icon(cfg, name, "VCS_BRANCH_ICON", NULL, &ic));
        esc(&content, g->branch);
    }
    if (!p9k_bool(cfg, "VCS_HIDE_TAGS", false) && tag[0]) {
        vcs_style(cfg, state, "TAG", &content);
        sb_chr(&content, ' '); sb_str(&content, p9k_icon(cfg, name, "VCS_TAG_ICON", NULL, &ic)); esc(&content, tag);
    }
    if (g->action[0]) {
        vcs_style(cfg, state, "ACTION", &content);
        sb_str(&content, " | "); esc(&content, g->action);
    } else {
        if (remote[0] && strcmp(remote, g->branch)) {
            vcs_style(cfg, state, "REMOTE_BRANCH", &content);
            sb_chr(&content, ' '); sb_str(&content, p9k_icon(cfg, name, "VCS_REMOTE_BRANCH_ICON", NULL, &ic)); esc(&content, remote);
        }
        if (g->staged || g->unstaged || untracked) {
            vcs_style(cfg, state, "DIRTY", &content);
            sb_str(&content, p9k_icon(cfg, name, "VCS_DIRTY_ICON", NULL, &ic));
            struct { int n; const char *part, *icon, *max; } k[] = {
                { g->staged, "STAGED", "VCS_STAGED_ICON", "VCS_STAGED_MAX_NUM" },
                { g->unstaged, "UNSTAGED", "VCS_UNSTAGED_ICON", "VCS_UNSTAGED_MAX_NUM" },
                { untracked, "UNTRACKED", "VCS_UNTRACKED_ICON", "VCS_UNTRACKED_MAX_NUM" },
            };
            for (int i = 0; i < 3; i++) {
                if (!k[i].n) continue;
                vcs_style(cfg, state, k[i].part, &content);
                sb_chr(&content, ' ');
                sb_str(&content, p9k_icon(cfg, name, k[i].icon, NULL, &ic));
                if (p9k_int(cfg, k[i].max, 1) != 1) sb_fmt(&content, "%d", k[i].n);
            }
        }
        if (behind > 0) {
            vcs_style(cfg, state, "INCOMING_CHANGES", &content);
            sb_chr(&content, ' '); sb_str(&content, p9k_icon(cfg, name, "VCS_INCOMING_CHANGES_ICON", NULL, &ic));
            if (bmax != 1) sb_fmt(&content, "%d", behind);
        }
        if (ahead > 0) {
            vcs_style(cfg, state, "OUTGOING_CHANGES", &content);
            sb_chr(&content, ' '); sb_str(&content, p9k_icon(cfg, name, "VCS_OUTGOING_CHANGES_ICON", NULL, &ic));
            if (amax != 1) sb_fmt(&content, "%d", ahead);
        }
        if (stashes > 0) {
            vcs_style(cfg, state, "STASH", &content);
            sb_chr(&content, ' '); sb_str(&content, p9k_icon(cfg, name, "VCS_STASH_ICON", NULL, &ic)); sb_fmt(&content, "%d", stashes);
        }
    }
    segment(c, name, VCS_STATE_BG[si], COLOR1, icon, sb_text(&content));
    sb_free(&content); sb_free(&ic);
}

static void seg_prompt_char(struct ctx *c) {
    bool err = c->st->status > 0;
    segment(c, err ? "prompt_prompt_char_ERROR_VIINS" : "prompt_prompt_char_OK_VIINS", COLOR1, err ? "196" : "76", "",
            "\xe2\x9d\xaf");
}

static const char *signame(int n) {
    static const char *const sig[] = { "EXIT", "HUP", "INT", "QUIT", "ILL", "TRAP", "ABRT", "BUS", "FPE", "KILL", "USR1",
        "SEGV", "USR2", "PIPE", "ALRM", "TERM", "STKFLT", "CHLD", "CONT", "STOP", "TSTP", "TTIN", "TTOU", "URG", "XCPU",
        "XFSZ", "VTALRM", "PROF", "WINCH", "POLL", "PWR", "SYS" };
    return n >= 1 && n < (int)(sizeof sig / sizeof sig[0]) ? sig[n] : NULL;
}

static void seg_status(struct ctx *c) {
    const struct p10k *cfg = c->cfg;
    int s = c->st->status < 0 ? 0 : c->st->status;  /* the first prompt: zsh's $? is 0 */
    c->side->emitted = true;                          /* off, it is still `_p9k_prompt_segment :` */
    char state[32], name[48], text[32];
    snprintf(state, sizeof state, "%s", s ? "ERROR" : "OK");
    if (p9k_bool(cfg, "STATUS_EXTENDED_STATES", false) && s > 128) strcat(state, "_SIGNAL");
    char var[48];
    snprintf(var, sizeof var, "STATUS_%s", state);
    if (!p9k_bool(cfg, var, true)) return;
    snprintf(text, sizeof text, "%d", s);
    if (s > 128 && !p9k_bool(cfg, "STATUS_HIDE_SIGNAME", false) && signame(s - 128)) {
        if (p9k_bool(cfg, "STATUS_VERBOSE_SIGNAME", true)) snprintf(text, sizeof text, "SIG%s(%d)", signame(s - 128), s - 128);
        else snprintf(text, sizeof text, "%s", signame(s - 128));
    }
    snprintf(name, sizeof name, "prompt_status_%s", state);
    bool verbose = p9k_bool(cfg, "STATUS_VERBOSE", true);
    if (s) {
        if (!p9k_bool(cfg, "STATUS_CROSS", false) && verbose) segment(c, name, "red", "yellow1", "CARRIAGE_RETURN_ICON", text);
        else segment(c, name, COLOR1, "red", "FAIL_ICON", "");
    } else if (verbose || p9k_bool(cfg, "STATUS_OK_IN_NON_VERBOSE", false)) {
        segment(c, name, COLOR1, "green", "OK_ICON", "");
    }
}

static void seg_execution_time(struct ctx *c) {
    const struct p10k *cfg = c->cfg;
    double d = c->st->duration;
    if (d < 0 || c->st->status < 0) return;
    if (d < p9k_float(cfg, "COMMAND_EXECUTION_TIME_THRESHOLD", 3)) return;
    char text[48];
    if (d < 60) {
        long prec = p9k_int(cfg, "COMMAND_EXECUTION_TIME_PRECISION", 2);
        if (!prec) snprintf(text, sizeof text, "%lds", (long)(d + 0.5));
        else snprintf(text, sizeof text, "%.*fs", (int)prec, d);
    } else {
        long t = (long)(d + 0.5);
        const char *fmt = p9k_param(cfg, "", "COMMAND_EXECUTION_TIME_FORMAT", "H:M:S");
        if (!strcmp(fmt, "H:M:S")) {
            if (t >= 36000) snprintf(text, sizeof text, "%ld:%02ld:%02ld", t / 3600, t / 60 % 60, t % 60);
            else if (t >= 3600) snprintf(text, sizeof text, "0%ld:%02ld:%02ld", t / 3600, t / 60 % 60, t % 60);
            else snprintf(text, sizeof text, "%02ld:%02ld", t / 60 % 60, t % 60);
        } else if (t >= 86400) snprintf(text, sizeof text, "%ldd %ldh %ldm %lds", t / 86400, t / 3600 % 24, t / 60 % 60, t % 60);
        else if (t >= 3600) snprintf(text, sizeof text, "%ldh %ldm %lds", t / 3600 % 24, t / 60 % 60, t % 60);
        else snprintf(text, sizeof text, "%ldm %lds", t / 60 % 60, t % 60);
    }
    segment(c, "prompt_command_execution_time", "red", "yellow1", "EXECUTION_TIME_ICON", text);
}

static void seg_background_jobs(struct ctx *c) {
    int j = c->st->jobs;
    c->side->emitted = true;                          /* the count is the prompt's to test */
    if (j <= 0) return;
    char msg[16] = "";
    if (p9k_bool(c->cfg, "BACKGROUND_JOBS_VERBOSE", true) && (j != 1 || p9k_bool(c->cfg, "BACKGROUND_JOBS_VERBOSE_ALWAYS", false)))
        snprintf(msg, sizeof msg, "%d", j);
    segment(c, "prompt_background_jobs", COLOR1, "cyan", "BACKGROUND_JOBS_ICON", msg);
}

static void seg_time(struct ctx *c) {
    segment(c, "prompt_time", COLOR2, COLOR1, "TIME_ICON", p9k_param(c->cfg, "", "TIME_FORMAT", "%D{%H:%M:%S}"));
}

/* prompt_context: %n@%m, or for DEFAULT_USER (not over ssh) the user alone;
 * the state is the connection's, or ROOT. */
static void seg_context(struct ctx *c) {
    const struct p10k *cfg = c->cfg;
    const struct p10k_state *st = c->st;
    const char *def_user = p10k_get(cfg, "_DEFAULT_USER"), *user = st->user ? st->user : "";
    struct sb text = { 0 };
    if (!p9k_bool(cfg, "ALWAYS_SHOW_CONTEXT", false) && def_user && def_user[0] && !st->ssh &&
        p9k_glob(def_user, strlen(def_user), user, strlen(user)))
        esc(&text, user);
    const char *state = st->ssh ? (st->sudo ? "REMOTE_SUDO" : "REMOTE") : st->sudo ? "SUDO" : "DEFAULT";
    if (st->root) state = "ROOT";
    if (!text.n) {
        char var[64];
        snprintf(var, sizeof var, "CONTEXT_%s_TEMPLATE", state);
        const char *t = p10k_get(cfg, var);
        p9k_decode_escapes(t ? t : p9k_param(cfg, "", "CONTEXT_TEMPLATE", "%n@%m"), &text);
    }
    char name[48];
    snprintf(name, sizeof name, "prompt_context_%s", state);
    segment(c, name, COLOR1, "yellow", "", sb_text(&text));
    sb_free(&text);
}

/* prompt_os_icon: the icon of the OS p10k recognises (_p9k_set_os), as the
 * content; on one it does not -- EmbLinkOS -- none, and nothing shows. */
static void seg_os_icon(struct ctx *c) {
    static const char *const known[][2] = { { "macos", "APPLE_ICON" }, { "linux", "LINUX_ICON" },
        { "android", "ANDROID_ICON" }, { "windows", "WINDOWS_ICON" }, { "freebsd", "FREEBSD_ICON" },
        { "solaris", "SUNOS_ICON" } };
    const char *os = c->st->os ? c->st->os : "";
    struct sb i = { 0 };
    for (size_t k = 0; k < sizeof known / sizeof known[0]; k++)
        if (!strcmp(os, known[k][0])) p9k_icon(c->cfg, "prompt_os_icon", known[k][1], NULL, &i);
    segment(c, "prompt_os_icon", "black", "white", "", sb_text(&i));
    sb_free(&i);
}

/* --- the directory: prompt_dir ---------------------------------------------- *
 * The path in parts, shortened by SHORTEN_STRATEGY, marked as p10k marks them
 * -- \1 where the delimiter goes, \2 after an anchor, \3 around
 * truncate_to_unique's short form -- then styled by those marks, joined by the
 * separator. */

static void dir_free(struct dirmodel *d) {
    for (int i = 0; i < DIR_PARTS; i++) { sb_free(&d->part[i].full); sb_free(&d->part[i].alt); }
    sb_free(&d->sep); sb_free(&d->header); sb_free(&d->footer);
}

static size_t nchars(const char *s) {
    size_t n = 0;
    for (; *s; s++) if (((unsigned char)*s & 0xc0) != 0x80) n++;
    return n;
}
/* The byte where character k (from 0) starts. */
static size_t char_at(const char *s, size_t k) {
    size_t i = 0;
    while (s[i] && k) {
        i++;
        while (s[i] && ((unsigned char)s[i] & 0xc0) == 0x80) i++;
        k--;
    }
    return i;
}
static int cells(const char *s) {
    int w = 0;
    for (size_t i = 0, n = strlen(s); i < n;) {
        uint32_t cp;
        int len = utf8_decode(s + i, (int)(n - i), &cp);
        i += (size_t)(len > 0 ? len : 1);
        int cw = wcwidth_cp(cp);
        if (cw > 0) w += cw;
    }
    return w;
}

/* Parts [from, to) replaced by one, `text` (NULL: none). */
static void parts_splice(struct dirmodel *d, int from, int to, const char *text) {
    for (int i = from; i < to; i++) { sb_free(&d->part[i].full); sb_free(&d->part[i].alt); }
    int keep = text ? 1 : 0, gone = to - from - keep;
    if (keep) {
        memset(&d->part[from], 0, sizeof d->part[from]);
        sb_str(&d->part[from].full, text);
    }
    memmove(&d->part[from + keep], &d->part[to], (size_t)(d->n - to) * sizeof d->part[0]);
    d->n -= gone;
    memset(&d->part[d->n], 0, (size_t)gone * sizeof d->part[0]);
}
static void part_set(struct dirpart *p, const char *text) {
    struct sb t = { 0 };
    sb_str(&t, text);
    sb_free(&p->full); sb_free(&p->alt);
    memset(p, 0, sizeof *p);
    p->full = t;
}

static const char DEFAULT_MARKERS[] = "(.bzr|.citc|.git|.hg|.node-version|.python-version|.ruby-version|"
                                      ".shorten_folder_marker|.svn|.terraform|CVS|Cargo.toml|composer.json|go.mod|package.json)";

struct marker_q { const char *pat; bool found; };
static void marker_each(void *arg, const char *name, bool dir) {
    struct marker_q *q = arg;
    (void)dir;
    if (!q->found && p9k_glob_name(q->pat, name)) q->found = true;
}
/* $dir/${~SHORTEN_FOLDER_MARKER}(#qN): anything there the pattern names. */
static bool has_marker(const struct ctx *c, const char *dir, const char *pat) {
    const struct p10k_fs *fs = c->st->fs;
    struct marker_q q = { pat, false };
    if (pat[0] && fs && fs->list) fs->list(fs->ctx, dir[0] ? dir : "/", marker_each, &q);
    return q.found;
}
struct prefix_q { const char *prefix; size_t n; int count; };
static void prefix_each(void *arg, const char *name, bool dir) {
    struct prefix_q *q = arg;
    if (dir && !strncmp(name, q->prefix, q->n)) q->count++;
}
/* $parent/$prefix*\/(N): the directories it begins. */
static int dirs_with_prefix(const struct ctx *c, const char *parent, const char *prefix, size_t n) {
    const struct p10k_fs *fs = c->st->fs;
    struct prefix_q q = { prefix, n, 0 };
    if (fs && fs->list) fs->list(fs->ctx, parent[0] ? parent : "/", prefix_each, &q);
    return q.count;
}

/* `text` with a pattern's replacement, as zsh's ${x/(#b)(*)\2/...}: what
 * follows applies p10k's in turn to a part's text. */
static bool ends_with(const struct sb *b, char c) { return b->n && b->p[b->n - 1] == c; }
static void drop_first(struct sb *b, char c) {
    char *at = b->p ? memchr(b->p, c, b->n) : NULL;
    if (!at) return;
    memmove(at, at + 1, (size_t)(b->p + b->n - at - 1));
    b->p[--b->n] = 0;
}
static void wrap(struct sb *b, const char *before, const char *after) {
    struct sb t = { 0 };
    sb_str(&t, before); sb_str(&t, sb_text(b)); sb_str(&t, after);
    sb_free(b);
    *b = t;
}

/* The URL of a file, as _p9k_url_escape writes it into a prompt. */
static void url_escape(const char *s, struct sb *out) {
    for (; *s; s++) {
        unsigned char ch = (unsigned char)*s;
        if (isalnum(ch) || strchr("/:_.-!'()~", ch)) sb_chr(out, (char)ch);
        else sb_fmt(out, "%%%%%02X", ch);
    }
}

static void seg_dir(struct ctx *c) {
    const struct p10k *cfg = c->cfg;
    const struct p10k_state *st = c->st;
    const char *cwd = st->pwd ? st->pwd : "/", *home = st->home;
    size_t hl = home ? strlen(home) : 0;
    /* the path: %~, or as it is (DIR_PATH_ABSOLUTE) */
    char p[1024];
    if (!p9k_bool(cfg, "DIR_PATH_ABSOLUTE", false) && hl > 1 && !strncmp(cwd, home, hl) && (!cwd[hl] || cwd[hl] == '/'))
        snprintf(p, sizeof p, "~%s", cwd + hl);
    else snprintf(p, sizeof p, "%s", cwd);
    struct dirmodel *d = calloc(1, sizeof *d);
    if (!d) return;
    for (const char *s = p;;) {                       /* ${(s:/:)p}: empty parts kept */
        const char *e = strchr(s, '/');
        size_t len = e ? (size_t)(e - s) : strlen(s);
        if (d->n < DIR_PARTS) sb_add(&d->part[d->n++].full, s, len);
        if (!e) break;
        s = e + 1;
    }
    bool fake_first = false, expand = false;
    const char *len_set = p10k_get(cfg, "SHORTEN_DIR_LENGTH");
    long shortenlen = len_set && len_set[0] ? strtol(len_set, NULL, 10) : -1;
    const char *delim_set = p10k_get(cfg, "SHORTEN_DELIMITER");
    struct sb delim = { 0 };
    if (delim_set) p9k_decode_escapes(delim_set, &delim);
    else sb_str(&delim, "\xe2\x80\xa6");
    long dlen = p9k_int(cfg, "SHORTEN_DELIMITER_LENGTH", -1);
    const char *strategy = p9k_param(cfg, "", "SHORTEN_STRATEGY", "");
    const char *markers = p9k_param(cfg, "", "SHORTEN_FOLDER_MARKER", DEFAULT_MARKERS);
    int n0 = d->n;

    if (!strcmp(strategy, "truncate_absolute") || !strcmp(strategy, "truncate_absolute_chars")) {
        long plen = (long)nchars(p), dl = dlen >= 0 ? dlen : p10k_width(sb_text(&delim), st);
        if (shortenlen > 0 && plen > shortenlen && plen > shortenlen + dl) {
            long left = shortenlen;
            for (int i = d->n - 1; i >= 0; i--) {
                const char *dir = sb_text(&d->part[i].full);
                long len = (long)nchars(dir) + (i > 0);
                if (len <= left) { left -= len; continue; }
                /* $dir[-n,-1]: the last n characters -- and with n = 0 all of
                 * them, as zsh reads -0 */
                struct sb t = { 0 };
                sb_chr(&t, '\001');
                sb_str(&t, left ? dir + char_at(dir, nchars(dir) - (size_t)left) : dir);
                part_set(&d->part[i], sb_text(&t));
                sb_free(&t);
                parts_splice(d, 0, i, NULL);
                break;
            }
        }
    } else if (!strcmp(strategy, "truncate_with_package_name") || !strcmp(strategy, "truncate_middle") ||
               !strcmp(strategy, "truncate_from_right")) {
        /* truncate_with_package_name names the package with jq, which the
         * system does not have: p10k without jq goes straight on */
        if (shortenlen > 0) {
            long dl = dlen >= 0 ? dlen : p10k_width(sb_text(&delim), st);
            long pref = shortenlen, suf = !strcmp(strategy, "truncate_middle") ? pref : 0;
            for (int i = 1; i < d->n - 1; i++) {
                const char *dir = sb_text(&d->part[i].full);
                long len = (long)nchars(dir);
                if (len <= pref + suf + dl) continue;
                struct sb t = { 0 };
                sb_add(&t, dir, char_at(dir, (size_t)pref));
                sb_chr(&t, '\001');
                sb_str(&t, dir + char_at(dir, (size_t)(len - suf)));
                part_set(&d->part[i], sb_text(&t));
                sb_free(&t);
            }
        }
    } else if (!strcmp(strategy, "truncate_to_last")) {
        long sl = len_set && len_set[0] ? shortenlen : 1;
        if (sl <= 0) sl = 1;
        if (d->n > sl + 1 || (p[0] != '/' && d->n > sl)) {
            fake_first = true;
            parts_splice(d, 0, d->n - (int)sl, NULL);
        }
    } else if (!strcmp(strategy, "truncate_to_first_and_last")) {
        if (shortenlen > 0) {
            long i = shortenlen + 1 + (p[0] == '/');
            for (; i <= d->n - shortenlen; i++) part_set(&d->part[i - 1], "\001");
        }
    } else if (!strcmp(strategy, "truncate_to_unique")) {
        expand = true;
        sb_reset(&delim);
        if (delim_set) p9k_decode_escapes(delim_set, &delim);
        else sb_str(&delim, "*");
        long sl = len_set && len_set[0] ? shortenlen : 1;
        if (sl < 0) sl = 1;
        /* DIR_TRUNCATE_BEFORE_MARKER: first or last[:N], with markers to find */
        char before[32] = "";
        const char *bm = p9k_param(cfg, "", "DIR_TRUNCATE_BEFORE_MARKER", "");
        if (!strcmp(bm, "first") || !strcmp(bm, "last")) snprintf(before, sizeof before, "%s:0", bm);
        else if ((!strncmp(bm, "first:", 6) || !strncmp(bm, "last:", 5))) {
            const char *num = strchr(bm, ':') + 1;
            if (*num == '-') num++;
            if (*num && strspn(num, "0123456789") == strlen(num)) snprintf(before, sizeof before, "%s", bm);
        }
        if (!markers[0]) before[0] = 0;
        int n = d->n;
        long i = 2, e = n - sl;                       /* 1-based, as p10k counts */
        struct sb orig_second = { 0 }, orig_last[DIR_PARTS];
        int nlast = 0;
        memset(orig_last, 0, sizeof orig_last);
        if (before[0]) {
            e += sl;
            if (n >= 2) sb_str(&orig_second, sb_text(&d->part[1].full));
            nlast = sl > n ? n : (int)sl;
            for (int k = 0; k < nlast; k++) sb_str(&orig_last[k], sb_text(&d->part[n - nlast + k].full));
        } else if (p[0] == '/') i++;
        /* the real directory holding part i: cwd less the parts from i on */
        char parent[1024];
        snprintf(parent, sizeof parent, "%s", cwd);
        for (long k = i; k <= n; k++) {
            char *slash = strrchr(parent, '/');
            if (!slash) { parent[0] = 0; break; }
            *slash = 0;
        }
        if (i - 1 >= 1 && i - 1 <= n && d->part[i - 2].full.n) sb_chr(&d->part[i - 2].full, '\002');
        int dl = dlen >= 0 ? (int)dlen : p10k_width(sb_text(&delim), st);
        for (; i <= e; i++) {
            struct dirpart *part = &d->part[i - 1];
            char rsub[256], dir[1300];
            snprintf(rsub, sizeof rsub, "%s", sb_text(&part->full));
            snprintf(dir, sizeof dir, "%s/%s", parent, rsub);
            if (markers[0] && has_marker(c, dir, markers)) sb_chr(&part->full, '\002');
            else {
                size_t len = nchars(rsub), j = 0;
                while (rsub[j] == '.') j++;
                j++;                                  /* rsub[(i)[^.]]: the first that is not a dot */
                for (; j + (size_t)dl < len; j++) {
                    size_t b = char_at(rsub, j);
                    if (dirs_with_prefix(c, parent, rsub, b) == 1) break;
                }
                size_t b = char_at(rsub, j);
                int saved = cells(rsub + (b < strlen(rsub) ? b : strlen(rsub))) - dl;
                if (saved > 0) {
                    part->shortenable = true;
                    part->saved = saved;
                    sb_chr(&part->alt, '\003');
                    sb_add(&part->alt, rsub, b);
                    sb_str(&part->alt, "\001\003");
                }
            }
            size_t pl = strlen(parent);
            snprintf(parent + pl, sizeof parent - pl, "/%s", rsub);
        }
        if (before[0]) {
            long at = 0;
            if (!strncmp(before, "last", 4)) { for (long k = n; k >= 1 && !at; k--) if (ends_with(&d->part[k - 1].full, '\002')) at = k; }
            else { at = n + 1; for (long k = 2; k <= n; k++) if (ends_with(&d->part[k - 1].full, '\002')) { at = k; break; } }
            long ee = at + strtol(strchr(before, ':') + 1, NULL, 10);
            if (ee > 1 && ee <= n) {
                parts_splice(d, 0, (int)ee - 1, NULL);
                fake_first = true;
            } else if (p[0] == '/' && p[1] && d->n >= 2) {
                part_set(&d->part[1], sb_text(&orig_second));
                sb_chr(&d->part[1].full, '\002');
            }
            for (int k = d->n < sl ? d->n : (int)sl; k > 0; k--) {
                struct dirpart *part = &d->part[d->n - k];
                if (ends_with(&part->full, '\002') || k > nlast) continue;
                part_set(part, sb_text(&orig_last[nlast - k]));
                sb_chr(&part->full, '\002');
            }
        } else {
            for (; i <= n; i++) sb_chr(&d->part[i - 1].full, '\002');
        }
        sb_free(&orig_second);
        for (int k = 0; k < nlast; k++) sb_free(&orig_last[k]);
    } else if (!strcmp(strategy, "truncate_with_folder_marker")) {
        if (markers[0]) {
            char dir[1024];
            snprintf(dir, sizeof dir, "%s", cwd);
            int m[DIR_PARTS + 1], nm = 0;
            for (int i = d->n - 1; i > 1; i--) {
                char *slash = strrchr(dir, '/');      /* ${dir:h} */
                if (slash == dir) dir[1] = 0;
                else if (slash) *slash = 0;
                if (has_marker(c, dir, markers)) m[nm++] = i;
            }
            m[nm++] = 1;
            for (int k = 0; k + 1 < nm; k++)
                if (m[k] - m[k + 1] > 2) parts_splice(d, m[k + 1], m[k] - 1, "\001");
        }
    } else if (shortenlen > 0) {
        int len = d->n - (d->part[0].full.n ? 0 : 1);
        if (len > shortenlen) parts_splice(d, 0, d->n - (int)shortenlen, "\001");
    }
    (void)n0;

    /* the state: writable or not (DIR_SHOW_WRITABLE), then the class */
    const char *wr = p9k_param(cfg, "", "DIR_SHOW_WRITABLE", "");
    int show = !strcmp(wr, "true") ? 1 : !strcmp(wr, "v2") ? 2 : !strcmp(wr, "v3") ? 3 : 0;
    int w = 0;
    if (show && st->fs && st->fs->writable && !st->fs->writable(st->fs->ctx, cwd)) {
        w = 1;
        if (show > 2 && st->fs->exists && !st->fs->exists(st->fs->ctx, cwd)) w = 2;
    }
    char state[96] = "prompt_dir", icon[96] = "";
    const char *classes = p10k_get(cfg, "DIR_CLASSES");
    struct sb defaults = { 0 };
    if (!classes) {                                   /* _p9k_init's four: /etc, home, below it, the rest */
        static const char *const def[][3] = { { "/etc|/etc/*", "ETC", "ETC_ICON" }, { "~", "HOME", "HOME_ICON" },
            { "~/*", "HOME_SUBFOLDER", "HOME_SUB_ICON" }, { "*", "DEFAULT", "FOLDER_ICON" } };
        for (int k = 0; k < 4; k++) {
            char seg[64];
            struct sb ic = { 0 };
            snprintf(seg, sizeof seg, "prompt_dir_%s", def[k][1]);
            p9k_icon(cfg, seg, def[k][2], NULL, &ic);
            sb_fmt(&defaults, "%s\037%s\037", def[k][0], def[k][1]);
            sb_str(&defaults, sb_text(&ic));
            sb_chr(&defaults, k < 3 ? '\037' : 0);
            sb_free(&ic);
        }
        classes = sb_text(&defaults);
    }
    {
        char ic[96];
        size_t cnt = p9k_array(classes, 0, ic, sizeof ic);
        for (size_t k = 0; k + 2 < cnt; k += 3) {
            char pat[256], cls[64];
            p9k_array(classes, k, pat, sizeof pat);
            p9k_array(classes, k + 1, cls, sizeof cls);
            p9k_array(classes, k + 2, ic, sizeof ic);
            /* [[ $_p9k__cwd == ${~a} ]]: a pattern, ~ at its start the home */
            struct sb full = { 0 };
            if (pat[0] == '~') { sb_str(&full, home && strcmp(home, "/") ? home : ""); sb_str(&full, pat + 1); }
            else sb_str(&full, pat);
            struct sb alt = { 0 };
            sb_chr(&alt, '(');
            sb_str(&alt, sb_text(&full));
            sb_chr(&alt, ')');
            bool match = p9k_glob(sb_text(&alt), alt.n, cwd, strlen(cwd));
            sb_free(&full); sb_free(&alt);
            if (!match) continue;
            if (cls[0]) {
                for (char *q = cls; *q; q++) *q = (char)toupper((unsigned char)*q);
                size_t at = strlen(state);
                snprintf(state + at, sizeof state - at, "_%.*s", (int)(sizeof state - at - 2), cls);
            }
            struct sb dec = { 0 };
            p9k_decode_escapes(ic, &dec);
            snprintf(icon, sizeof icon, "\001%s", sb_text(&dec));
            sb_free(&dec);
            break;
        }
    }
    sb_free(&defaults);
    if (w) {
        if (show == 1) snprintf(state, sizeof state, "prompt_dir_NOT_WRITABLE");
        else strncat(state, w == 2 ? "_NON_EXISTENT" : "_NOT_WRITABLE", sizeof state - strlen(state) - 1);
        snprintf(icon, sizeof icon, "LOCK_ICON");
    }
    char col[16];
    struct sb style = { 0 };
    sb_str(&style, "%b");
    p9k_color(cfg, state, "BACKGROUND", "blue", col); bg(&style, col);
    p9k_color(cfg, state, "FOREGROUND", COLOR1, col); fg(&style, col);
    const char *sty = sb_text(&style);

    /* parts=("${(@)parts//\%/%%}"), in both forms */
    for (int i = 0; i < d->n; i++)
        for (int f = 0; f < 2; f++) {
            struct sb *b = f ? &d->part[i].alt : &d->part[i].full;
            if (!b->n || !memchr(b->p, '%', b->n)) continue;
            struct sb t = { 0 };
            esc(&t, sb_text(b));
            sb_free(b);
            *b = t;
        }
    /* HOME_FOLDER_ABBREVIATION, or DIR_OMIT_FIRST_CHARACTER */
    struct sb abbr = { 0 };
    p9k_decode_escapes(p9k_param(cfg, "", "HOME_FOLDER_ABBREVIATION", "~"), &abbr);
    if (strcmp(sb_text(&abbr), "~") && !fake_first && p[0] == '~' && (!p[1] || p[1] == '/') && d->n) {
        struct sb t = { 0 };
        if (expand) esc(&t, sb_text(&abbr)); else sb_str(&t, sb_text(&abbr));
        if (has_pct(sb_text(&t))) sb_str(&t, sty);
        part_set(&d->part[0], sb_text(&t));
        sb_free(&t);
    } else if (p9k_bool(cfg, "DIR_OMIT_FIRST_CHARACTER", false) && !fake_first && d->n > 1 && !d->part[0].full.n &&
               d->part[1].full.n) {
        parts_splice(d, 0, 1, NULL);
    }
    sb_free(&abbr);
    /* the last part's style (PATH_HIGHLIGHT) */
    struct sb last = { 0 }, anchor = { 0 };
    if (!strcmp(p9k_param(cfg, state, "PATH_HIGHLIGHT_BOLD", ""), "true")) sb_str(&last, "%B");
    if (p9k_isset(cfg, "DIR_PATH_HIGHLIGHT_FOREGROUND")) { p9k_color(cfg, state, "PATH_HIGHLIGHT_FOREGROUND", "", col); fg(&last, col); }
    if (last.n && d->n) {
        struct dirpart *lp = &d->part[d->n - 1];
        for (int f = 0; f < 2; f++) {
            struct sb *b = f ? &lp->alt : &lp->full;
            if (f && !lp->shortenable) continue;
            struct sb t = { 0 };
            sb_str(&t, sb_text(&last));
            for (const char *q = sb_text(b); *q; q++) { sb_chr(&t, *q); if (*q == '\001') sb_str(&t, sb_text(&last)); }
            sb_str(&t, sty);
            sb_free(b);
            *b = t;
        }
    }
    /* anchors (ANCHOR_BOLD, ANCHOR_FOREGROUND): the parts ending in \2 */
    if (!strcmp(p9k_param(cfg, state, "ANCHOR_BOLD", ""), "true")) sb_str(&anchor, "%B");
    if (p9k_isset(cfg, "DIR_ANCHOR_FOREGROUND")) { p9k_color(cfg, state, "ANCHOR_FOREGROUND", "", col); fg(&anchor, col); }
    for (int i = 0; i < d->n; i++)
        for (int f = 0; f < 2; f++) {
            struct sb *b = f ? &d->part[i].alt : &d->part[i].full;
            bool lastpart = i == d->n - 1;
            if (anchor.n && !(last.n && lastpart) && ends_with(b, '\002')) {
                b->p[--b->n] = 0;
                wrap(b, sb_text(&anchor), sty);
            } else drop_first(b, '\002');
        }
    /* the delimiter, and SHORTENED_FOREGROUND for what was shortened */
    struct sb dl = { 0 };
    if (expand) esc(&dl, sb_text(&delim)); else sb_str(&dl, sb_text(&delim));
    if (p9k_isset(cfg, "DIR_SHORTENED_FOREGROUND")) {
        struct sb sfg = { 0 };
        p9k_color(cfg, state, "SHORTENED_FOREGROUND", "", col);
        fg(&sfg, col);
        if (has_pct(sb_text(&dl))) { sb_str(&dl, sty); sb_str(&dl, sb_text(&sfg)); }
        for (int i = 0; i < d->n; i++)
            for (int f = 0; f < 2; f++) {
                struct sb *b = f ? &d->part[i].alt : &d->part[i].full;
                const char *t = sb_text(b), *one = strrchr(t, '\001');
                if (!one) continue;
                /* \3(*)\1(*)\3, else (*)\1(*): the whole part in the shortened colour */
                struct sb r = { 0 };
                const char *s3 = strchr(t, '\003');
                const char *e3 = s3 ? strchr(one, '\003') : NULL;
                if (s3 && e3 && s3 < one) {
                    sb_add(&r, t, (size_t)(s3 - t));
                    sb_str(&r, sb_text(&sfg));
                    sb_add(&r, s3 + 1, (size_t)(one - s3 - 1));
                    sb_str(&r, sb_text(&dl));
                    sb_add(&r, one + 1, (size_t)(e3 - one - 1));
                    sb_str(&r, sty);
                    sb_str(&r, e3 + 1);
                } else {
                    sb_str(&r, sb_text(&sfg));
                    sb_add(&r, t, (size_t)(one - t));
                    sb_str(&r, sb_text(&dl));
                    sb_str(&r, one + 1);
                    sb_str(&r, sty);
                }
                sb_free(b);
                *b = r;
            }
        sb_free(&sfg);
    } else {
        if (has_pct(sb_text(&dl))) sb_str(&dl, sty);
        for (int i = 0; i < d->n; i++)
            for (int f = 0; f < 2; f++) {
                struct sb *b = f ? &d->part[i].alt : &d->part[i].full;
                const char *t = sb_text(b), *one = strchr(t, '\001');
                struct sb r = { 0 };
                if (one) { sb_add(&r, t, (size_t)(one - t)); sb_str(&r, sb_text(&dl)); sb_str(&r, one + 1); }
                else sb_str(&r, t);
                sb_free(b);
                *b = r;
                for (char *q; b->p && (q = memchr(b->p, '\003', b->n));) { memmove(q, q + 1, (size_t)(b->p + b->n - q - 1)); b->p[--b->n] = 0; }
            }
    }
    sb_free(&dl); sb_free(&last); sb_free(&anchor);
    /* the separator */
    if (!strcmp(cwd, "/") && p9k_bool(cfg, "DIR_OMIT_FIRST_CHARACTER", false)) sb_str(&d->sep, "/");
    else {
        if (p9k_isset(cfg, "DIR_PATH_SEPARATOR_FOREGROUND")) { p9k_color(cfg, state, "PATH_SEPARATOR_FOREGROUND", "", col); fg(&d->sep, col); }
        struct sb ps = { 0 };
        p9k_decode_escapes(p9k_param(cfg, state, "PATH_SEPARATOR", "/"), &ps);
        if (expand) esc(&d->sep, sb_text(&ps)); else sb_str(&d->sep, sb_text(&ps));
        sb_free(&ps);
        if (has_pct(sb_text(&d->sep))) sb_str(&d->sep, sty);
    }
    /* DIR_HYPERLINK: the directory as a link to itself */
    if (p9k_bool(cfg, "DIR_HYPERLINK", false) && cwd[0] == '/') {
        sb_str(&d->header, "%{\033]8;;file://");
        url_escape(cwd, &d->header);
        sb_str(&d->header, "\a%}");
        sb_str(&d->footer, "%{\033]8;;\a%}");
    }
    d->expand = expand;
    /* the content: every part full, or for the budget's line the parts it
     * chose short; a second directory in one prompt all short */
    struct sb content = { 0 };
    int upto = 0;
    if (expand) {
        if (c->dir && !c->has_dir && !c->dir_before) upto = c->dir->upto;
        else upto = d->n;
    }
    for (int pass = 0; pass < 2; pass++) {
        sb_reset(&content);
        sb_str(&content, sb_text(&d->header));
        for (int i = 0; i < d->n; i++) {
            if (i) sb_str(&content, sb_text(&d->sep));
            const struct dirpart *dp = &d->part[i];
            sb_str(&content, sb_text(dp->shortenable && i < (pass ? upto : 0) ? &dp->alt : &dp->full));
        }
        sb_str(&content, sb_text(&d->footer));
        if (!pass) d->width = p10k_width(sb_text(&content), st);
    }
    if (expand && c->dir && !c->has_dir && !c->dir_before) {
        d->upto = c->dir->upto;
        dir_free(c->dir);
        *c->dir = *d;                                 /* the line's budget works from it */
        c->has_dir = true;
    } else dir_free(d);
    free(d);
    sb_free(&delim);
    segment(c, state, "blue", COLOR1, icon, sb_text(&content));
    sb_free(&content); sb_free(&style);
}

/* --- the segment list ------------------------------------------------------------ */

static const char *const DRAWN[] = { "dir", "vcs", "prompt_char", "status", "command_execution_time",
    "background_jobs", "time", "context", "os_icon", "newline" };
static const char *const KNOWN[] = { "direnv", "asdf", "virtualenv", "anaconda", "pyenv", "goenv", "nodenv", "nvm",
    "nodeenv", "node_version", "go_version", "rust_version", "dotnet_version", "php_version", "laravel_version",
    "java_version", "package", "rbenv", "rvm", "fvm", "luaenv", "jenv", "plenv", "perlbrew", "phpenv", "scalaenv",
    "haskell_stack", "kubecontext", "terraform", "terraform_version", "aws", "aws_eb_env", "azure", "gcloud",
    "google_app_cred", "toolbox", "nordvpn", "ranger", "yazi", "nnn", "lf", "xplr", "vim_shell", "midnight_commander",
    "nix_shell", "chezmoi_shell", "vi_mode", "todo", "timewarrior", "taskwarrior", "per_directory_history", "cpu_arch",
    "load", "disk_usage", "ram", "swap", "public_ip", "vpn_ip", "ip", "proxy", "wifi", "example", "root_indicator",
    "history", "user", "host", "ssh", "vcs_joined", "dir_writable", "detect_virt", "openfoam", "rspec_stats",
    "symfony2_tests", "symfony2_version", "docker_machine", "battery", "_p9k_internal_nothing" };

bool p10k_segment_drawn(const char *name) {
    for (size_t i = 0; i < sizeof DRAWN / sizeof DRAWN[0]; i++) if (!strcmp(DRAWN[i], name)) return true;
    return false;
}
bool p10k_segment_known(const char *name) {
    if (p10k_segment_drawn(name)) return true;
    for (size_t i = 0; i < sizeof KNOWN / sizeof KNOWN[0]; i++) if (!strcmp(KNOWN[i], name)) return true;
    return !strncmp(name, "custom_", 7);
}

static void draw(struct ctx *c, const char *name) {
    if (!strcmp(name, "_p9k_internal_nothing")) sb_reset(&c->side->sss);   /* ${_p9k__sss::=}: no end symbol */
    else if (!strcmp(name, "dir")) seg_dir(c);
    else if (!strcmp(name, "vcs")) seg_vcs(c);
    else if (!strcmp(name, "prompt_char")) seg_prompt_char(c);
    else if (!strcmp(name, "status")) seg_status(c);
    else if (!strcmp(name, "command_execution_time")) seg_execution_time(c);
    else if (!strcmp(name, "background_jobs")) seg_background_jobs(c);
    else if (!strcmp(name, "time")) seg_time(c);
    else if (!strcmp(name, "context")) seg_context(c);
    else if (!strcmp(name, "os_icon")) seg_os_icon(c);
}

/* --- the lines ----------------------------------------------------------------- */

#define MAX_LINES 8
#define MAX_SEGS 128
struct lines {
    int n;
    int nleft[MAX_LINES], nright[MAX_LINES];
    char left[MAX_LINES][MAX_SEGS][40], right[MAX_LINES][MAX_SEGS][40];
};

static void split_lines(const struct p10k *cfg, struct lines *L) {
    memset(L, 0, sizeof *L);
    const char *lv = p10k_get(cfg, "LEFT_PROMPT_ELEMENTS"), *rv = p10k_get(cfg, "RIGHT_PROMPT_ELEMENTS");
    if (!lv) lv = "context dir vcs";
    if (!rv) rv = "status root_indicator background_jobs history time";
    char e[40];
    char left[MAX_SEGS * 2][40], right[MAX_SEGS * 2][40];
    int nl = 0, nr = 0;
    size_t cnt = p9k_array(lv, 0, e, sizeof e);
    for (size_t i = 0; i < cnt && nl < MAX_SEGS * 2 - 2; i++) p9k_array(lv, i, left[nl++], sizeof left[0]);
    if (p9k_bool(cfg, "PROMPT_ON_NEWLINE", false)) { snprintf(left[nl++], 40, "newline"); snprintf(left[nl++], 40, "_p9k_internal_nothing"); }
    cnt = p9k_array(rv, 0, e, sizeof e);
    for (size_t i = 0; i < cnt && nr < MAX_SEGS * 2; i++) p9k_array(rv, i, right[nr++], sizeof right[0]);
    int lnl = 1, rnl = 1;
    for (int i = 0; i < nl; i++) lnl += !strcmp(left[i], "newline");
    for (int i = 0; i < nr; i++) rnl += !strcmp(right[i], "newline");
    int lines = lnl > rnl ? lnl : rnl;
    if (lines > MAX_LINES) lines = MAX_LINES;
    /* fewer lines on one side: the left's newlines go in front; the right's at
     * the end, or in front with RPROMPT_ON_NEWLINE */
    int lpad = lines - lnl, rpad = lines - rnl;
    bool rfront = p9k_bool(cfg, "RPROMPT_ON_NEWLINE", false);
    L->n = lines;
    int line = lpad;
    for (int i = 0; i < nl && line < lines; i++) {
        if (!strcmp(left[i], "newline")) { line++; continue; }
        if (L->nleft[line] < MAX_SEGS) memcpy(L->left[line][L->nleft[line]++], left[i], 40);
    }
    line = rfront ? rpad : 0;
    for (int i = 0; i < nr && line < lines; i++) {
        if (!strcmp(right[i], "newline")) { line++; continue; }
        if (L->nright[line] < MAX_SEGS) memcpy(L->right[line][L->nright[line]++], right[i], 40);
    }
}

/* One side of one line: its segments drawn in order, and the side's end. */
static void build_side(const struct p10k *cfg, const struct p10k_state *st, int cols, bool right,
                       char segs[][40], int nsegs, struct dirmodel *dir, bool dir_before, bool *has_dir,
                       bool *emitted, struct sb *out) {
    struct side sd = { 0 };
    sd.none = true;
    struct sb tmp = { 0 }, ret = { 0 };
    if (!right) {
        p9k_icon(cfg, "", "LEFT_SEGMENT_SEPARATOR", NULL, &tmp);
        p9k_icon(cfg, "prompt_empty_line", "LEFT_PROMPT_LAST_SEGMENT_END_SYMBOL", sb_text(&tmp), &ret);
        sb_str(&sd.sss, "%f"); sb_str(&sd.sss, sb_text(&ret));
    } else {
        p9k_decode_escapes(p9k_param(cfg, "", "EMPTY_LINE_RIGHT_PROMPT_FIRST_SEGMENT_START_SYMBOL", ""), &sd.sss);
    }
    struct ctx c = { cfg, st, cols, right, out, &sd, 0, 0, false, dir, false, dir_before };
    int join_from = 0;
    for (int i = 0; i < nsegs; i++) {
        char name[40];
        snprintf(name, sizeof name, "%s", segs[i]);
        size_t ln = strlen(name);
        bool joined = ln > 7 && !strcmp(name + ln - 7, "_joined");
        if (joined) name[ln - 7] = 0;
        else join_from = i;
        c.index = i;
        c.joined = joined;
        c.join_from = join_from;
        draw(&c, name);
    }
    if (!right) {
        sb_str(out, "%b%k"); sb_str(out, sb_text(&sd.sss)); sb_str(out, "%b%k%f");
    } else {
        sb_str(out, sb_text(&sd.sss)); sb_str(out, "%b%k%f");
    }
    if (has_dir) *has_dir = c.has_dir;
    if (emitted) *emitted = sd.emitted;
    sb_free(&sd.s); sb_free(&sd.ss); sb_free(&sd.sss); sb_free(&sd.w); sb_free(&tmp); sb_free(&ret);
}

/* LEFT_SEGMENT_END_SEPARATOR, after a line's left side. */
static void end_separator(const struct p10k *cfg, struct sb *left) {
    struct sb es = { 0 };
    p9k_icon(cfg, "", "LEFT_SEGMENT_END_SEPARATOR", NULL, &es);
    if (sb_text(&es)[0]) { sb_str(left, sb_text(&es)); sb_str(left, "%b%k%f"); }
    sb_free(&es);
}

static void frame_icon(const struct p10k *cfg, const char *name, struct sb *out) {
    p9k_icon(cfg, "", name, NULL, out);
    if (sb_text(out)[0] && has_pct(sb_text(out))) sb_str(out, "%b%k%f");
}

bool p10k_render(const struct p10k *cfg, const struct p10k_state *st, int cols, struct p10k_prompt *out) {
    memset(out, 0, sizeof *out);
    struct lines *L = malloc(sizeof *L);
    if (!L) return false;
    split_lines(cfg, L);
    int ind = 1;                                      /* ZLE_RPROMPT_INDENT */
    struct sb prompt = { 0 }, rprompt = { 0 };
    bool on_newline = p9k_bool(cfg, "PROMPT_ON_NEWLINE", false);
    /* the ruler: a row of RULER_CHAR above every prompt but the first (one
     * cell wide, or a space; a space with no background is an empty line) */
    if (p9k_bool(cfg, "SHOW_RULER", false) && !st->first) {
        struct sb ch = { 0 }, z = { 0 };
        p9k_icon(cfg, "", "RULER_CHAR", NULL, &ch);
        if (p10k_width(sb_text(&ch), st) != 1 || nchars(sb_text(&ch)) != 1) { sb_reset(&ch); sb_chr(&ch, ' '); }
        char c1[16], c2[16];
        p9k_color(cfg, "prompt_ruler", "BACKGROUND", "", c1);
        if (!c1[0] && !strcmp(sb_text(&ch), " ")) sb_str(&z, "\n");
        else {
            p9k_color(cfg, "prompt_ruler", "FOREGROUND", "", c2);
            sb_str(&z, "%b"); bg(&z, c1); fg(&z, c2);
            for (int i = 0; i < cols - ind; i++) sb_str(&z, sb_text(&ch));
            sb_str(&z, "%k%f\n");
        }
        p9k_zexpand(sb_text(&z), st, &prompt);
        sb_free(&ch); sb_free(&z);
    }
    struct dirmodel *dir = calloc(1, sizeof *dir);
    if (!dir) { free(L); sb_free(&prompt); return false; }
    bool dir_before = false;                          /* a directory took a line's budget already */
    for (int line = 0; line < L->n; line++) {
        bool last = line == L->n - 1;
        dir_free(dir);
        memset(dir, 0, sizeof *dir);
        struct sb left = { 0 }, right = { 0 };
        bool has_dir = false;
        bool right_emitted = false;
        build_side(cfg, st, cols, false, L->left[line], L->nleft[line], dir, dir_before, &has_dir, NULL, &left);
        build_side(cfg, st, cols, true, L->right[line], L->nright[line], NULL, true, NULL, &right_emitted, &right);
        /* frames: the first and last lines' prefixes and suffixes */
        struct sb pre = { 0 }, suf = { 0 };
        if (L->n > 1) {
            if (line == 0 && (p9k_isset(cfg, "MULTILINE_FIRST_PROMPT_PREFIX") || on_newline)) frame_icon(cfg, "MULTILINE_FIRST_PROMPT_PREFIX", &pre);
            else if (last && (p9k_isset(cfg, "MULTILINE_LAST_PROMPT_PREFIX") || on_newline)) frame_icon(cfg, "MULTILINE_LAST_PROMPT_PREFIX", &pre);
            else if (line > 0 && !last && (p9k_isset(cfg, "MULTILINE_NEWLINE_PROMPT_PREFIX") || on_newline)) frame_icon(cfg, "MULTILINE_NEWLINE_PROMPT_PREFIX", &pre);
            if (line == 0) frame_icon(cfg, "MULTILINE_FIRST_PROMPT_SUFFIX", &suf);
            else if (last) frame_icon(cfg, "MULTILINE_LAST_PROMPT_SUFFIX", &suf);
            else frame_icon(cfg, "MULTILINE_NEWLINE_PROMPT_SUFFIX", &suf);
        }
        /* LEFT_SEGMENT_END_SEPARATOR after the last line's left side (or the
         * one before, with PROMPT_ON_NEWLINE) */
        bool end_sep = line == L->n - 1 - (on_newline ? 1 : 0);
        if (end_sep) end_separator(cfg, &left);
        struct sb lfull = { 0 }, rfull = { 0 };
        sb_str(&lfull, sb_text(&pre)); sb_str(&lfull, sb_text(&left));
        /* a right side: when a segment put code there, or the line is never
         * without one (_p9k_line_never_empty_right) -- the start symbol of an
         * empty right side, or a frame's suffix */
        struct sb empty_start = { 0 };
        p9k_decode_escapes(p9k_param(cfg, "", "EMPTY_LINE_RIGHT_PROMPT_FIRST_SEGMENT_START_SYMBOL", ""), &empty_start);
        bool never_empty = empty_start.n && (!last || p10k_width(sb_text(&empty_start), st) > 0);
        sb_free(&empty_start);
        if (L->n > 1 && suf.n && (!last || p10k_width(sb_text(&suf), st) > 0)) never_empty = true;
        bool right_shown = right_emitted || never_empty;
        if (right_shown) { sb_str(&rfull, sb_text(&right)); sb_str(&rfull, sb_text(&suf)); }
        /* the directory's budget, as _p9k_set_prompt computes it: the free
         * columns m less those the command line is to keep (h), shortening the
         * parts left to right while that is negative */
        if (has_dir) {
            int m = cols - p10k_width(sb_text(&lfull), st) - p10k_width(sb_text(&rfull), st) - ind - 1;
            long a = p9k_int(cfg, "DIR_MIN_COMMAND_COLUMNS", 40);
            double pct = p9k_float(cfg, "DIR_MIN_COMMAND_COLUMNS_PCT", 50);
            double h = 0;
            if (last && (a > 0 || pct > 0)) { double f = 0.01 * pct * cols; h = a < f ? f : (double)a; }
            const char *maxl = p9k_param(cfg, "", "DIR_MAX_LENGTH", "0");
            size_t digits = strspn(maxl, "0123456789");
            if (digits && (!maxl[digits] || (maxl[digits] == '%' && !maxl[digits + 1]))) {
                bool pctlim = maxl[digits] == '%';
                double lim = pctlim ? dir->width - 0.01 * atof(maxl) * cols : dir->width - atof(maxl);
                if ((pctlim || lim > 0) && h < m + lim) h = m + lim;
            }
            double d = m - h;
            int upto = 0;
            for (int i = 0; i < dir->n && d < 0; i++) {
                if (!dir->part[i].shortenable) continue;
                d += dir->part[i].saved;
                upto = i + 1;
            }
            if (upto) {
                sb_free(&left); sb_free(&lfull);
                memset(&left, 0, sizeof left); memset(&lfull, 0, sizeof lfull);
                dir_free(dir);
                memset(dir, 0, sizeof *dir);
                dir->upto = upto;
                build_side(cfg, st, cols, false, L->left[line], L->nleft[line], dir, dir_before, &has_dir, NULL, &left);
                if (end_sep) end_separator(cfg, &left);
                sb_str(&lfull, sb_text(&pre)); sb_str(&lfull, sb_text(&left));
            }
            dir_before = true;
        }
        if (!last) {
            p9k_zexpand(sb_text(&lfull), st, &prompt);
            int m = cols - p10k_width(sb_text(&lfull), st) - p10k_width(sb_text(&rfull), st) - ind - 1;
            if (right_shown && m >= 0) {               /* _p9k_build_gap_post; m < 0: no right side */
                const char *kind = line == 0 ? "FIRST" : "NEWLINE";
                char var[64], seg[64], c1[16], c2[16];
                struct sb ch = { 0 }, gap = { 0 }, fill = { 0 };
                snprintf(var, sizeof var, "MULTILINE_%s_PROMPT_GAP_CHAR", kind);
                p9k_icon(cfg, "", var, NULL, &ch);
                if (!ch.n || p10k_width(sb_text(&ch), st) != 1 || nchars(sb_text(&ch)) != 1) { sb_reset(&ch); sb_chr(&ch, ' '); }
                snprintf(seg, sizeof seg, "prompt_multiline_%s_prompt_gap", line == 0 ? "first" : "newline");
                p9k_color(cfg, seg, "BACKGROUND", "", c1);
                p9k_color(cfg, seg, "FOREGROUND", "", c2);
                bool styled = c1[0] || c2[0];
                if (c1[0]) bg(&gap, c1);
                if (c2[0]) fg(&gap, c2);
                for (int i = 0; i <= m; i++) sb_str(&fill, sb_text(&ch));
                snprintf(var, sizeof var, "MULTILINE_%s_PROMPT_GAP_EXPANSION", kind);
                const char *exp = p10k_get(cfg, var);
                if (exp) { p9k_expand_gap(exp, sb_text(&fill), &gap); styled = true; }
                else sb_str(&gap, sb_text(&fill));
                sb_str(&gap, sb_text(&rfull));
                if (styled) sb_str(&gap, "%b%k%f");
                p9k_zexpand(sb_text(&gap), st, &prompt);
                sb_free(&ch); sb_free(&gap); sb_free(&fill);
            }
            sb_chr(&prompt, '\n');
        } else {
            p9k_zexpand(sb_text(&lfull), st, &prompt);
            if (right_shown) p9k_zexpand(sb_text(&rfull), st, &rprompt);
        }
        sb_free(&left); sb_free(&right); sb_free(&pre); sb_free(&suf); sb_free(&lfull); sb_free(&rfull);
    }
    out->lines = L->n;
    free(L);
    dir_free(dir);
    free(dir);

    /* the transient prompt: ❯ by the last status, as p10k builds it */
    struct sb tr = { 0 };
    const char *tp = p9k_param(cfg, "", "TRANSIENT_PROMPT", "off");
    bool same = st->last_prompt_pwd && st->pwd && !strcmp(st->last_prompt_pwd, st->pwd);
    if (!strcmp(tp, "always") || (!strcmp(tp, "same-dir") && same)) {
        struct sb z = { 0 }, x = { 0 };
        bool err = st->status > 0;
        char cc[16];
        const char *seg = err ? "prompt_prompt_char_ERROR_VIINS" : "prompt_prompt_char_OK_VIINS";
        p9k_color(cfg, seg, "FOREGROUND", err ? "196" : "76", cc);
        sb_str(&z, "%b%k%s%u");
        fg(&z, cc);
        p9k_expand_param(cfg, p9k_param(cfg, seg, "CONTENT_EXPANSION", "${P9K_CONTENT}"), "\xe2\x9d\xaf", "", st, &x);
        sb_str(&z, sb_text(&x));
        sb_str(&z, "%b%k%f%s%u");
        struct sb es = { 0 };
        p9k_icon(cfg, "", "LEFT_SEGMENT_END_SEPARATOR", NULL, &es);
        sb_str(&z, sb_text(&es));
        if (strcmp(sb_text(&es), "") && strcmp(sb_text(&es), " ")) sb_str(&z, "%b%k%f");
        p9k_zexpand(sb_text(&z), st, &tr);
        sb_free(&z); sb_free(&x); sb_free(&es);
    }
    bool oom = prompt.oom || rprompt.oom || tr.oom;
    out->left = prompt.p ? prompt.p : calloc(1, 1);
    out->right = rprompt.p ? rprompt.p : calloc(1, 1);
    out->transient = tr.p ? tr.p : calloc(1, 1);
    out->add_newline = p9k_bool(cfg, "PROMPT_ADD_NEWLINE", false) && !st->first ? (int)p9k_int(cfg, "PROMPT_ADD_NEWLINE_COUNT", 1) : 0;
    if (out->add_newline < 0) out->add_newline = 0;
    if (oom || !out->left || !out->right || !out->transient) { p10k_prompt_free(out); return false; }
    return true;
}

void p10k_prompt_free(struct p10k_prompt *p) {
    free(p->left); free(p->right); free(p->transient);
    p->left = p->right = p->transient = NULL;
}
