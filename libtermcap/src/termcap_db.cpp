// termcap_db.cpp -- a small built-in termcap database.
//
// bash's termcap reads /etc/termcap, which does not exist on macOS and many
// other modern systems.  To stay self-contained we ship compiled-in entries
// for the terminals that matter for line editing.  tgetent() consults, in
// order: a $TERMCAP entry, a $TERMCAP/`/etc/termcap` file if present, then
// this database.
//
// Each entry is a single physical line: `names:cap:cap:...:`.  Names are
// separated by `|`; ESC is written as a literal \033 and other control bytes
// in caret (^X) notation, both understood by tgetst1().

namespace gnash::termcap {

const char *builtin_database =
    // Minimal dumb terminal.
    "dumb|80-column dumb tty:co#80:am:bl=^G:cr=^M:do=^J:sf=^J:\n"

    // DEC VT100.
    "vt100|vt100-am|dec vt100:"
    "co#80:li#24:am:xn:"
    "cr=^M:do=^J:le=^H:nd=\033[C:up=\033[A:sf=^J:bl=^G:ta=^I:"
    "cl=\033[;H\033[2J:cm=\033[%i%d;%dH:ho=\033[H:"
    "ce=\033[K:cd=\033[J:"
    "so=\033[7m:se=\033[m:us=\033[4m:ue=\033[m:md=\033[1m:me=\033[m:mr=\033[7m:"
    "ku=\033OA:kd=\033OB:kr=\033OC:kl=\033OD:kh=\033[H:"
    "ks=\033[?1h\033=:ke=\033[?1l\033>:\n"

    // Generic ANSI / PC.
    "ansi|ansi/pc-term compatible:"
    "co#80:li#24:am:"
    "cr=^M:do=\033[B:le=^H:nd=\033[C:up=\033[A:bl=^G:ta=^I:"
    "cl=\033[H\033[J:cm=\033[%i%d;%dH:ho=\033[H:ce=\033[K:cd=\033[J:"
    "so=\033[7m:se=\033[0m:us=\033[4m:ue=\033[0m:md=\033[1m:me=\033[0m:mr=\033[7m:"
    "DO=\033[%dB:LE=\033[%dD:RI=\033[%dC:UP=\033[%dA:"
    "ku=\033[A:kd=\033[B:kr=\033[C:kl=\033[D:kh=\033[H:\n"

    // xterm and the family of emulators that behave like it.
    "xterm|xterm-256color|xterm-color|xterm-16color|screen|screen-256color|"
    "tmux|tmux-256color|vt220|linux:"
    "co#80:li#24:am:km:mi:ms:xn:"
    "cr=^M:do=^J:le=^H:nd=\033[C:up=\033[A:bl=^G:ta=^I:sf=^J:sr=\033M:"
    "DO=\033[%dB:UP=\033[%dA:LE=\033[%dD:RI=\033[%dC:"
    "ce=\033[K:cd=\033[J:cl=\033[H\033[2J:cm=\033[%i%d;%dH:ho=\033[H:"
    "ct=\033[3g:st=\033H:"
    "dc=\033[P:DC=\033[%dP:ic=\033[@:IC=\033[%d@:"
    "al=\033[L:AL=\033[%dL:dl=\033[M:DL=\033[%dM:"
    "im=\033[4h:ei=\033[4l:"
    "so=\033[7m:se=\033[27m:us=\033[4m:ue=\033[24m:"
    "md=\033[1m:me=\033[0m:mr=\033[7m:vb=\033[?5h\033[?5l:"
    "ks=\033[?1h\033=:ke=\033[?1l\033>:"
    "ku=\033OA:kd=\033OB:kr=\033OC:kl=\033OD:kh=\033OH:@7=\033OF:"
    "kD=\033[3~:kI=\033[2~:kN=\033[6~:kP=\033[5~:kb=^H:"
    "k1=\033OP:k2=\033OQ:k3=\033OR:k4=\033OS:"
    "k5=\033[15~:k6=\033[17~:k7=\033[18~:k8=\033[19~:k9=\033[20~:\n";

}  // namespace gnash::termcap
