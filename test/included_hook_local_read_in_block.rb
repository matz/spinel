# A local of an included hook read inside its `base.class_eval do ... end`
# block: as a statement of the block, by a block inside it, and by
# define_method blocks, each includer with its own value.
module HlPlain
  def self.included(base)
    opts = [1, 2]
    label = "for #{base.name}"
    base.class_eval do
      p opts
      opts.each { |o| p [label, o] }
      x = opts.size
      p x
    end
  end
end
class HlA; include HlPlain; end

module HlCounted
  def self.included(base)
    n = base.name.size
    base.class_eval do
      define_method(:size_of_name) { n }
      define_method(:twice) { n * 2 }
    end
  end
end
class HlAb; include HlCounted; end
class HlAbcd; include HlCounted; end
p HlAb.new.size_of_name, HlAbcd.new.size_of_name, HlAbcd.new.twice

module HlDefaults
  @opts = { mode: :fast }
  def self.included(base)
    opts = @opts
    base.class_eval do
      define_method(:mode) { opts[:mode] }
    end
  end
end
class HlB; include HlDefaults; end
p HlB.new.mode
