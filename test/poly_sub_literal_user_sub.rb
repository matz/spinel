# A boxed receiver's sub with a Regexp literal where a class of the program
# defines sub too: the per-class dispatch serves it, its String arm the
# builtin, and a handle slot takes the dispatch's String.
class Pathish
  def initialize(s)
    @s = s
  end

  def sub(*args, &block)
    "P:" + @s.sub(*args, &block)
  end

  def to_s
    @s
  end
end

class Version
  def initialize(v)
    @version = v
  end

  def prerelease?
    @version.match?(/[a-zA-Z]/)
  end

  def canonical_segments
    @canonical_segments ||= begin
      canonical_version = @version.sub(/(?<=[a-zA-Z.])[.0]+\z/, "")
      canonical_version.sub!(/(?<=\.|\A)[0.]+(?=[a-zA-Z])/, "") if prerelease?
      canonical_version.split(".")
    end
  end
end

v = Version.new(ARGV.empty? ? "1.2.0.0" : ARGV[0])
p v.canonical_segments
w = Version.new(ARGV.empty? ? "1.0.a.0" : ARGV[0].to_sym)
p w.canonical_segments

def tail(path, to)
  return to if to
  rest = path.sub(/\(\.:format\)$/, "")
  rest.start_with?("/") ? rest.delete_prefix("/") : rest
end
[":id(.:format)", "/users(.:format)"].each { |x| puts tail(x, nil) }
puts tail(ARGV.empty? ? "/a(.:format)" : ARGV[0], ARGV[1])

# the receiver can hold the class of the program: its own sub answers
[Pathish.new("a0"), "b0", Pathish.new("c0")].each { |x| puts x.sub(/0/, "1") }
