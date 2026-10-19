/* iOS defaults.  An iOS app has no command line and no shell environment, so the inputs the
   desktop build takes from argv and HP_* variables default to the app sandbox:

     <bundle>/image.bin            recompiled image (bundled with the app)
     Documents/<name>.iso          the disc image (copied in by the user with Finder file sharing)
     Documents/disc/               extracted disc files (only read with HP_IOP=hle)
     Documents/memcard0.ps2        memory card in slot 1
     Documents/hp.log              stderr (unless HP_STDERR is set)

   HP_GS defaults to gpu.  Variables that are set (SIMCTL_CHILD_HP_*, devicectl
   --environment-variables) still win. */
#include <TargetConditionals.h>
#if TARGET_OS_IOS
#include <SDL3/SDL.h>
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

#define ISO_NAME "Harry Potter and the Prisoner of Azkaban (USA).iso"

static char docs[1024], image[1200], iso[1400], memcard[1200], disc[1200];

static int is_file(const char *p) {
    struct stat st;
    return stat(p, &st) == 0 && S_ISREG(st.st_mode) && st.st_size > 0;
}

/* Documents/ISO_NAME, else the first *.iso in Documents (Finder keeps whatever name it had). */
static int find_iso(void) {
    snprintf(iso, sizeof iso, "%s/%s", docs, ISO_NAME);
    if (is_file(iso)) return 1;
    DIR *d = opendir(docs);
    if (!d) return 0;
    struct dirent *e;
    int found = 0;
    while (!found && (e = readdir(d))) {
        size_t n = strlen(e->d_name);
        if (n > 4 && !strcasecmp(e->d_name + n - 4, ".iso")) {
            snprintf(iso, sizeof iso, "%s/%s", docs, e->d_name);
            found = is_file(iso);
        }
    }
    closedir(d);
    return found;
}

/* Blocks until an ISO is in Documents (Retry) or the user quits. */
static void wait_for_iso(void) {
    static const char msg[] =
        "Copy your own disc image of the game into this app's Documents folder, then tap Retry.\n\n"
        "On a Mac: Finder, select the iPhone, Files tab, drag the .iso onto \"HP Azkaban\".\n\n"
        "Expected name: " ISO_NAME;
    const SDL_MessageBoxButtonData buttons[2] = {
        {SDL_MESSAGEBOX_BUTTON_RETURNKEY_DEFAULT, 1, "Retry"},
        {SDL_MESSAGEBOX_BUTTON_ESCAPEKEY_DEFAULT, 0, "Quit"},
    };
    const SDL_MessageBoxData box = {SDL_MESSAGEBOX_INFORMATION, NULL, "Game disc not found", msg, 2, buttons, NULL};
    while (!find_iso()) {
        int hit = 1;
        fprintf(stderr, "[ios] no .iso in %s\n", docs);
        if (!SDL_ShowMessageBox(&box, &hit)) { fprintf(stderr, "[ios] message box: %s\n", SDL_GetError()); SDL_Delay(2000); continue; }
        if (hit != 1) exit(0);
    }
}

const char *hp_ios_setup(void) {
    const char *home = getenv("HOME");
    snprintf(docs, sizeof docs, "%s/Documents", home ? home : ".");
    mkdir(docs, 0755);
    if (!getenv("HP_STDERR")) {   /* Documents/hp.log, readable through Finder file sharing */
        char log[1200];
        snprintf(log, sizeof log, "%s/hp.log", docs);
        if (freopen(log, "w", stderr)) setvbuf(stderr, NULL, _IONBF, 0);
    }
    if (chdir(docs) != 0) fprintf(stderr, "[ios] cannot enter %s\n", docs);   /* relative paths (HP_WAV=rec.wav, HP_SHOT=shots) land in Documents */
    {   /* Documents/options.txt: one NAME=VALUE per line, set as environment variables before the
           defaults below (e.g. HP_GPUPOINT=1 for nearest-texel sampling, HP_NOUPLOADSKIP=1);
           a line starting with # is a comment.  Edit it from a Mac through Finder file sharing. */
        char path[1200];
        snprintf(path, sizeof path, "%s/options.txt", docs);
        FILE *f = fopen(path, "r");
        if (f) {
            char line[512];
            while (fgets(line, sizeof line, f)) {
                char *p = line;
                while (*p == ' ' || *p == '\t') p++;
                if (*p == '#' || *p == '\n' || !*p) continue;
                char *eq = strchr(p, '=');
                if (!eq) continue;
                *eq = 0;
                char *v = eq + 1, *nl = strpbrk(v, "\r\n");
                if (nl) *nl = 0;
                setenv(p, v, 1);
                fprintf(stderr, "[ios] option %s=%s\n", p, v);
            }
            fclose(f);
        }
    }
    setenv("HP_GS", "gpu", 0);
    setenv("HP_FPSLOG", "1", 0);   /* a [fps] line in hp.log every 5 s */
    /* VU1 microprograms the recompiled set does not have yet run in the interpreter (slow); keep
       their code images in Documents/vu1 so they can be fetched from the device and recompiled */
    mkdir("vu1", 0755);
    setenv("HP_VUCOLLECT", "vu1", 0);
    snprintf(memcard, sizeof memcard, "%s/memcard0.ps2", docs);
    setenv("HP_MEMCARD", memcard, 0);
    snprintf(disc, sizeof disc, "%s/disc", docs);
    if (!getenv("HP_ISO")) {
        wait_for_iso();
        setenv("HP_ISO", iso, 1);
    }
    const char *base = SDL_GetBasePath();
    snprintf(image, sizeof image, "%simage.bin", base ? base : "./");
    fprintf(stderr, "[ios] image %s\n[ios] iso %s\n[ios] memcard %s\n", image, getenv("HP_ISO"), getenv("HP_MEMCARD"));
    return image;
}

const char *hp_ios_disc_dir(void) { return disc; }
#endif
