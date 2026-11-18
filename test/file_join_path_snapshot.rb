# spinel: gc-minor
# File.join consumes a String segment before the next to_path can mutate it.
class LaterPath
  def initialize(first)
    @first = first
  end

  def to_path
    @first << ("x" * 1_000)
    GC.start
    "leaf"
  end
end

first = +"root"
p File.join([first, LaterPath.new(first)]).bytesize
