# A program that gives some class a respond_to? of its own, by define_method or
# by alias, takes the include_all argument itself: a call with a flag that is
# no literal hands the flag's value over, not true or false.
class C
  define_method(:respond_to?) { |n, all = false| p [:own, all]; true }
end
class D
  def my_rt(n, all = false) = (p [:alias, all]; true)
  alias respond_to? my_rt
end
f = ARGV.empty? ? :yes : nil
p C.new.respond_to?(:zz, f)
g = ARGV.empty? ? 7 : nil
p D.new.respond_to?(:zz, g)
