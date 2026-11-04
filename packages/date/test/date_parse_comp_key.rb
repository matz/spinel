# Date._parse never hands back its internal century marker, whichever way
# `comp` goes.
require "date"
p Date._parse("2026-11-04 10:20:30.25 +09:00", false).key?(:_comp)
p Date._parse("2026-11-04 10:20:30", false).key?(:_comp)
p Date._parse("2026-11-04", true).key?(:_comp)
p Date._parse("26-11-04", true)
p Date._parse("26-11-04", false)
