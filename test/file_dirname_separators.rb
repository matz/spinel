# File.dirname drops the separators at the end of the path before it takes
# the directory, and the separators between the directory and the last
# part, as CRuby does: "/usr/bin/" is in "/usr", "a/b///c" in "a/b".
p File.dirname("/usr/bin/"), File.dirname("/usr/bin//"), File.dirname("a/b/"), File.dirname("a/b///c")
p File.dirname("/usr//bin"), File.dirname("//usr"), File.dirname("a///"), File.dirname("a//b//")
p File.dirname("/"), File.dirname("//"), File.dirname("///"), File.dirname("."), File.dirname("a"), File.dirname("/a")
p File.dirname(""), File.dirname("a/"), File.dirname("/a/"), File.dirname("./a/"), File.dirname("../a/b/")
