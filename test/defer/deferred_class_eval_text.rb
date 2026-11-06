# Compiled with --defer-refusals: a String class_eval or module_eval the
# compiler cannot expand (an interpolation it does not read, a String only
# the run time has) raises NotImplementedError where it runs, after the
# class body's statements ahead of it, not skipped as a class-body
# declaration so that its methods are silently missing. Without the flag it
# is refused.
# spinel: defer-refusals: 3:start body true true after
puts "start"
begin
  class A
    M = :g
    def g = 2
    puts "body"
    class_eval <<~RUBY, __FILE__, __LINE__ + 1
      def f = #{M}
    RUBY
  end
rescue NotImplementedError => e
  puts e.message.include?("class_eval")
end
module Mod
  def self.add(src) = module_eval(src)
end
begin
  Mod.add("def h = 4")
rescue NotImplementedError => e
  puts e.message.include?("module_eval")
end
puts "after"
proc { "a" }.ruby2_keywords
puts "not reached"
