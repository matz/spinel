# A class's own `def self.include` that calls super: the builtin include
# would mix the module in while the program runs, and Spinel fixes a
# class's ancestors at compile time. Refused, not compiled without the
# module.
module M
  def hi = :m
end
class Base
  def self.include(*k)
    puts "hook #{k.inspect}"
    super
  end
end
class Kid < Base
  include M
end
p Kid.new.respond_to?(:hi)
