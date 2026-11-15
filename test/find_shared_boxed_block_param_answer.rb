# find over a typed Array whose block parameter another block holds boxed,
# when the block answers its element: the answer is tested by Ruby's truth
# rules, for String, Integer and Float elements, with and without ifnone,
# and through next. find answers the element it yielded, also when the block
# rebinds the parameter or stores over the entry.
class Entry
  def initialize(tags)
    @tags = tags.to_s.split("; ")
    @lens = @tags.map { |x| x.length }
    @vals = @lens.map { |x| x.to_f }
  end
  def tags; @tags; end
  def lens; @lens; end
  def vals; @vals; end
end
class Book
  PART = /\A(\w+)(?:\s\(([^)]*)\))?/
  def initialize(entries); @entries = entries; end
  def self.parse(text)
    s = text.nil? ? "" : text.strip
    new([Entry.new(s[PART, 2])])
  end
  def self.tagged(entries); entries.each { |t| return t unless t.tags.empty? }; nil; end
  def answers; answers_of(Book.tagged(@entries)); end
  def answers_of(e)
    return [] if e.nil?
    [e.tags.find { |c| c }, e.tags.find(-> { "none" }) { |c| c },
     e.tags.find(-> { "none" }) { |c| next c }, e.lens.find { |c| c },
     e.vals.find { |c| c }, e.lens.find(-> { -1 }) { |c| c },
     e.vals.find(-> { -1.0 }) { |c| c },
     e.tags.find { |c| c = "other"; true }, e.tags.find(-> { "none" }) { |c| c = "other"; c },
     e.tags.find { |c| e.tags[0] = "replacement"; true },
     e.tags.find(-> { "none" }) { |c| e.tags[0] = "again"; true }, e.tags[0]]
  end
end
mixed = [1, "alpha (one; two; three) beta (x)", nil]
p Book.parse(mixed[ARGV.size + 1]).answers
class Bag
  def each; yield 1; end
end
Bag.new
class Note
  def tags; 1; end
  def lens; 1; end
  def vals; 1; end
end
note = Note.new
p [note.tags, note.lens, note.vals]
p note.instance_variable_get(:@h) if ARGV.size > 5
