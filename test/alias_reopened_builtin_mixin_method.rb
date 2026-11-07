# An alias in a class whose original is a method of a builtin module it
# includes, when the program reopens that module (as a library does with
# `module Enumerable`): the builtin's methods are still there, so the alias
# raises no NameError where it runs.

module Enumerable
  def many2? = count > 1
end

module Comparable
  def cmp_tag = :cmp
end

module Extra
  def extra = :extra
end

class Jar
  include Enumerable, Extra
  def initialize(h) = @h = h
  def each(&block) = @h.each(&block)
  alias :to_hash :to_h
  alias_method :entries2, :entries
end

class Ver
  include Comparable
  attr_reader :n
  def initialize(n) = @n = n
  def <=>(o) = n <=> o.n
  alias :within? :between?
end

puts "loaded"
j = Jar.new("a" => 1, "b" => 2)
p j.many2?
p j.extra
p j.map { |k, v| "#{k}=#{v}" }
p Ver.new(2).cmp_tag
p Ver.new(2) > Ver.new(1)

# an original that nothing has is still a NameError
begin
  class Jar
    alias :nothing2 :nothing
  end
rescue NameError => e
  p e.message
end
