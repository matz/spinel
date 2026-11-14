# A NUL in the format String of format, String#% and Time#strftime is a byte
# like any other: what follows it is formatted too.
p format("%s\0%s", 1, 2)
p "a\0b%s" % "c"
p Time.at(0).utc.strftime("a\0b%Y")
f = "x\0%d"
p format(f, 5)
t = Time.at(0).utc
g = "%Y\0%m"
p t.strftime(g)
p t.strftime("%Y-%m-%d")
p t.asctime
