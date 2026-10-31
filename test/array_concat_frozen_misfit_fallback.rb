# spinel: gc-stress
# A concrete converter beside a known non-Array operand retains the
# existing misfit route: argument effects run, then frozen entry wins.
$concat_fallback_events = []
class ConcatFallbackSource
  def to_ary
    $concat_fallback_events << :conversion
    [2]
  end
end
def concat_fallback_argument
  $concat_fallback_events << :argument
  3
end
a = [1].freeze
begin
  a.concat(ConcatFallbackSource.new, concat_fallback_argument)
rescue => e
  p e.class
end
p $concat_fallback_events
p a
$concat_fallback_events = []
a = [1].freeze
begin
  r = a.concat(ConcatFallbackSource.new, concat_fallback_argument)
  p r
rescue => e
  p e.class
end
p $concat_fallback_events
p a
