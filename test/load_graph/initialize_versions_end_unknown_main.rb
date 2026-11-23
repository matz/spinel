# A class defined in two files, each with its own initialize, where the
# record cannot tell which file the class ends with: one is required under
# a condition and again from a third file. A call of new ran the table's
# initialize (the first file's), while the class's other methods went
# through their version entries and ran the last file's; the object
# lacked what the last initialize sets. new now runs the initialize's
# entry. A subclass with an initialize of its own, calling super, and a
# class value read at run time, take it too.
require_relative "initialize_versions_end_unknown_flash" if ARGV.empty?
require_relative "initialize_versions_end_unknown_dispatch"
require_relative "initialize_versions_end_unknown_any_key"
f = Flash.new
f["a"] = 1
p f.discard
class Note < Flash
  def initialize(t)
    super()
    @t = t
  end
  def t = [@t, discard]
end
p Note.new(:x).t
k = [Flash, Note][ARGV.size]
p (k == Note ? k.new(:y) : k.new).discard
