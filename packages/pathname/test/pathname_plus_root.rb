# Pathname#+ at a root, as CRuby's Pathname#plus joins: an absolute argument
# replaces the receiver, a ".." takes off the receiver's names and drops at
# the root, and the rest of the argument is joined as written. The root is a
# drive's "C:/" where the platform has drives (native Windows, whose
# Pathname#+ is its own) and "/" elsewhere, so each line compares against it
# and prints the same on both.
require "pathname"

ROOT = File.absolute_path?("C:/") ? "C:/" : "/"

def plus(a, b) = (Pathname.new(a) + b).to_s

p plus("#{ROOT}a/b", "../..") == ROOT
p plus("#{ROOT}a/b", "../../../c") == "#{ROOT}c"
p plus("#{ROOT}a/b", "../c/") == "#{ROOT}a/c/"
p plus("#{ROOT}a", "b/./c") == "#{ROOT}a/b/./c"
p plus(ROOT, "a") == "#{ROOT}a"
p plus(ROOT, "..") == ROOT
p plus("x/y", "#{ROOT}z") == "#{ROOT}z"
p plus("#{ROOT}a", "#{ROOT}b") == "#{ROOT}b"
p ((Pathname.new("#{ROOT}a") + "b" + "../c") / "d").to_s == "#{ROOT}a/c/d"

# and away from any root, as on every platform
p plus("a", "b/../c")
p plus("D", "sub/./.")
p plus("a/b", "..")
p plus("a", "./")
p plus("a/b", "../../../")
p plus("", "q")
