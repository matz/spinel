# find over an Array of Strings whose block parameter another block holds
# boxed. The call's receiver was boxed when the builtin's copy was put
# beside it (Book.tagged answers an Entry only once the rounds settle), so
# the copy binds the shared parameter from its boxed walk; the receiver
# then narrows to the Array the String transforms give. find's own loop
# declared the parameter as the element (const char *) and the body read
# it boxed, so the C did not build.
class Entry
  def initialize(tags); @tags = tags.to_s.split("; "); end
  def tags; @tags; end
end
class Book
  PART = /\A(\w+)(?:\s\(([^)]*)\))?/
  def initialize(entries); @entries = entries; end
  def self.parse(text)
    s = text.nil? ? "" : text.strip
    new([Entry.new(s[PART, 2])])
  end
  def self.tagged(entries); entries.each { |t| return t unless t.tags.empty? }; nil; end
  def label; label_of(Book.tagged(@entries)); end
  def label_of(entry); return nil if entry.nil?; entry.tags.find { |c| c.match?(/\At/) }; end
end
mixed = [1, "alpha (one; two; three) beta (x)", nil]
puts Book.parse(mixed[ARGV.size + 1]).label
class Bag
  def each; yield 1; end
end
Bag.new
class Note
  def tags; 1; end
end
note = Note.new
puts note.tags
puts note.instance_variable_get(:@h).inspect if ARGV.size > 5
