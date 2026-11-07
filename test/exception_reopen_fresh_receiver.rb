# spinel: gc-stress
# A reopened exception method reached by the runtime class of a fresh
# exception (`RuntimeError.new("x").tag`), where another call made the
# method's dispatch pick among the reopenings: the exception was held by
# nothing while the method ran, and one that allocates before reading self
# ("RT:" + super) read it freed (SPINEL_GC_STRESS=2). It is now rooted for
# the call.

class StandardError
  def tag = "SE:#{message}"
end

class RuntimeError
  def tag = "RT:" + super
end

class ArgumentError
  def tag = "AE<" + super + ">"
end

begin; raise ArgumentError, "a"; rescue => e; puts e.tag; end
puts RuntimeError.new("x").tag
puts ArgumentError.new("y" * 3).tag
puts StandardError.new("z").tag
puts (1..20).map { |i| RuntimeError.new("n#{i}").tag.size }.sum
