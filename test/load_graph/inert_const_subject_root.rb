# A `case` subject built by the program is held only by its temp while the
# `when` values are evaluated; a value that autoloads runs a file that
# allocates, so the subject is rooted across it (under GC stress it was
# collected, and the arm compared another String).
autoload :LGC, File.join(__dir__, "inert_const_lazy_gc_lib")
autoload :LGS, File.join(__dir__, "inert_const_lazy_gs_lib")
n = 41
case "x" + n.to_s
when LGC then p :int
when "x41" then p :match
else p :none
end
case "y" + n.to_s
when LGS then p :str
when "y41" then p :match2
else p :none2
end
