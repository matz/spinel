# spinel: gc-stress
class FreshFormat
  def to_s
    GC.start
    +"<%s/%d>"
  end
  alias to_str to_s
end

def make_args
  [FreshFormat.new, +"tail", 17]
end

p format(*make_args)
