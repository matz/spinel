require "tmpdir"

class WriteOperand
  def to_s
    puts "convert"
    GC.start
    +"a\0b"
  end
end

def operand
  puts "argument"
  WriteOperand.new
end

Dir.mktmpdir do |dir|
  path = File.join(dir, "data")
  file = File.open(path, "w+b")
  streams = [file, 0]
  operands = [WriteOperand.new, nil]

  p file.write(operands[0])
  file.flush
  p streams[0].syswrite(operands[0])
  p file.syswrite(WriteOperand.new)
  p file.write(operand, operand)
  file.rewind
  p file.read.bytes
  file.close
end
