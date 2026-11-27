# A program that reopens Module (a core extension giving every module a
# method) and makes modules at run time: Module.new in a Hash's default block
# and in a map. The value of Module.new is a class value, as it is where
# Module is not reopened; typed as an instance of the reopened class, the
# generated C initialized an object pointer from the class value.
class Module
  def tag = "tag:#{name || "anon"}"
end

class MethodSet
  METHOD_CACHES = Hash.new { |h, k| h[k] = Module.new }
  def self.cache_for(ns) = METHOD_CACHES[ns]
  def self.poison(k, v) = METHOD_CACHES[k] = v
end

MethodSet.poison("s", 1)
a = MethodSet.cache_for(:a)
p a.class, a.equal?(MethodSet.cache_for(:a)), a.equal?(MethodSet.cache_for(:b)), MethodSet.cache_for("s")

mods = [1, 2].map { Module.new }
p mods.size, mods[0].class, mods[0].equal?(mods[1])
