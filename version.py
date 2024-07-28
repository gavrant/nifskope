import os.path as os_path

APP_VER_MAJOR    = 2
APP_VER_MINOR    = 0
APP_VER_REVISION = 9
APP_VER_BUILD    = 1

APP_VER_SHORT = f"{APP_VER_MAJOR}.{APP_VER_MINOR}.{APP_VER_REVISION}.{APP_VER_BUILD}"

build_suffix = ""
if APP_VER_BUILD > 0:
    if APP_VER_BUILD > 26:
        raise Exception(f"The build value ({APP_VER_BUILD}) is too high")
    build_suffix = chr(97 + APP_VER_BUILD - 1) # 'a' + (APP_VER_BUILD - 1)
APP_VER_FULL = f"{APP_VER_MAJOR}.{APP_VER_MINOR} Dev {APP_VER_REVISION}{build_suffix} (Gavrant)"

APP_NAME    = "NifSkope"
APP_COMPANY = "NifTools"


# Read git build hash
def get_git_hash_path(git_head_path):
    with open(git_head_path, "r") as f:
        ref_parts = f.readline().split(' ', 1)
        if len(ref_parts) == 2 and ref_parts[0] == "ref:":
            return ref_parts[1].strip()
        else:
            raise Exception(f"Failed to read '{git_head_path}'")

git_hash_path = get_git_hash_path(".git/HEAD")

def get_git_build(git_hash_path):
    GIT_BUILD_LENGTH = 7
    with open(git_hash_path, "r") as f:
        hash_line = f.readline().strip()
        if len(hash_line) >= GIT_BUILD_LENGTH:
            # Single component, hopefully the commit hash
            # Fetch first seven characters (abbreviated hash)
            return hash_line[:GIT_BUILD_LENGTH]
        else:
            raise Exception(f"Failed to read hash from '{git_hash_path}'")

APP_GIT_BUILD = get_git_build(".git/" + git_hash_path)


# Generate src/version.h
script_path = os_path.realpath(__file__)
header_path = os_path.realpath("src/version.h")
with open(header_path, "w") as header:
    def header_string(s):
        outs = ""
        for c in s:
            if c in ("\"", "\\", "'", "?"):
                outs += "\\" + c
            elif c >= " " and c <= "~":
                outs += c
            else:
                chcode = ord(c)
                if chcode == 0x0a:
                    outs += "\\n"
                elif chcode == 0x0d:
                    outs += "\\r"
                elif chcode == 0x09:
                    outs += "\\t"
                else:
                    raise Exception(f"Unsupported character in string '{s}'")
        return "\"" + outs + "\""

    header_dir = os_path.split(header_path)[0]
    header_script_path = os_path.relpath(script_path, start=header_dir).replace("\\", "/")
    header.write(f"// DO NOT EDIT BY HAND. Generated by {header_script_path}.\n")
    header.write("\n")
    header.write( "#ifndef VERSION_H\n")
    header.write( "#define VERSION_H\n")
    header.write( "\n")
    header.write(f"#define APP_VER_MAJOR    {APP_VER_MAJOR}\n")
    header.write(f"#define APP_VER_MINOR    {APP_VER_MINOR}\n")
    header.write(f"#define APP_VER_REVISION {APP_VER_REVISION}\n")
    header.write(f"#define APP_VER_BUILD    {APP_VER_BUILD}\n")
    header.write( "\n")
    header.write(f"#define APP_VER_SHORT {header_string(APP_VER_SHORT)}\n")
    header.write(f"#define APP_VER_FULL  {header_string(APP_VER_FULL)}\n")
    header.write( "\n")
    header.write(f"#define APP_GIT_BUILD {header_string(APP_GIT_BUILD)}\n")
    header.write( "\n")
    header.write(f"#define APP_COMPANY   {header_string(APP_COMPANY)}\n")
    header.write(f"#define APP_NAME      {header_string(APP_NAME)}\n")
    header.write(f"#define APP_NAME_FULL {header_string(APP_NAME + " " + APP_VER_FULL)}\n")
    header.write( "\n")
    header.write( "#endif // VERSION_H\n")


# Update text files
def replace_text(in_path, out_path, replacements):
    with open(in_path, "r") as inf:
        in_lines = inf.readlines()
    # Force Unix newline ("\n") on write because it's the standard for text files in this repo
    with open(out_path, "w", newline = "\n") as outf:
        for inline in in_lines:
            outline = inline
            for replace_what, replace_with in replacements:
                outline = outline.replace(replace_what, replace_with)
            outf.write(outline)

replace_text("build/README.md.in", "README.md", [("@VERSION@", APP_VER_FULL), ])


print("Done")
