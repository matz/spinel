# spinel: gc-stress
module InputPair
  extend self

  def report(label, bytes)
    bytes << "!"
    p [label, bytes.start_with?("# spinel:"), bytes.end_with?("!"), bytes.encoding.name]
  end

  def read(label)
    source = __FILE__
    report(label, File.binread(source))
  end
end

InputPair.read("first")
InputPair.read("second")
