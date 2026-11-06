# A reopening that includes a module the builtin already has (Time and
# Comparable, IO, File, Dir and Enumerable) adds nothing to its ancestors,
# as in CRuby. And a Dir subject in `when` or `in` is a Dir and an
# Enumerable when it is not nil.
module M; end
class IO; include Enumerable; end
class File; include Enumerable; end
class Dir; include Enumerable; include M; end
class Time; include Comparable; end
p Dir.ancestors.take(4), Time.ancestors.take(3)
p IO.ancestors.count(Enumerable), File.ancestors.count(Enumerable)
p Dir.ancestors.count(Enumerable), Time.ancestors.count(Comparable)

d = Dir.new(".")
case d when Dir then p 1 when Enumerable then p 2 else p 0 end
case d when Enumerable then p 2 else p 0 end
case d
in Enumerable then p 2
else p 0
end
case d
in Dir then p 1
else p 0
end
e = nil
e = Dir.new(".") if ARGV.size > 5
case e when Enumerable then p 2 when Dir then p 1 else p 0 end
case e
in Dir then p 1
else p 0
end
x = [d, nil][ARGV.size]
case x when Dir then p 1 else p 0 end
case x when Enumerable then p 2 else p 0 end
d.close
