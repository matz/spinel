# spinel: gc-stress
class AffixNeedle
  def initialize(target, events, label, replacement, text)
    @target, @events, @label, @replacement, @text = target, events, label, replacement, text
  end

  def to_str
    @events << @label
    @target.replace(@replacement)
    @text
  end
end

class AffixSubclass < String
  def to_str
    raise "a String subclass supplies its own bytes"
  end
end

def note_affix(events, label, value)
  events << label
  value
end

def has_affix_prefix(r, events, first, second, third)
  note_affix(events, :receiver, r).start_with?(
    note_affix(events, :first, first), note_affix(events, :second, second), note_affix(events, :third, third))
end

def has_affix_suffix(r, events, first, second)
  note_affix(events, :receiver, r).end_with?(note_affix(events, :first, first), note_affix(events, :second, second))
end

text = +"original"
boxed = [text, 1].first
events = []
miss = [AffixNeedle.new(text, events, :miss, "zz" * 1024, "q"), 1].first
match = [AffixNeedle.new(text, events, :match, "y" * 4096, "y"), 1].first
skip = [AffixNeedle.new(text, events, :skip, "other", "other"), 1].first

p has_affix_prefix(boxed, events, miss, match, skip), events, text.bytesize
events.clear
p has_affix_suffix(boxed, events, match, skip), events, text.bytesize
p has_affix_prefix(boxed, events, "no", [/\Ay/, 1].first, skip), events
p boxed.start_with?(AffixSubclass.new("y")), boxed.end_with?(AffixSubclass.new("y"))
