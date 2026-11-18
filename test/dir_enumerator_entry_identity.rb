# spinel: gc-minor
# A saved directory entry is the same String when fetched again.
require "tmpdir"

def mark_entry(entries)
  index = entries.index("record")
  entry = entries[index]
  suffix = "!" * 256
  entry << suffix
  p [entry.equal?(entries[index]), entries[index] == "record" + suffix]
end

Dir.mktmpdir do |dir|
  File.write(File.join(dir, "record"), "")
  mark_entry(Dir.each_child(dir).to_a)
  mark_entry(Dir.foreach(dir).to_a)
end
