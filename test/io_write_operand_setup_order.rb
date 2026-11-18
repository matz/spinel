# A later IO#write argument's block runs after the earlier argument.
def mark(log, text)
  log << text
  text
end
log = []
reader, writer = IO.pipe
p (if ENV["SPINEL_WRITE_RECEIVER"]
     log << "alternate"
     writer
   else
     log << "receiver"
     writer
   end).write(mark(log, "first"), ["second"].map { |word| mark(log, word) }.join)
writer.close
p log, reader.read
reader.close
