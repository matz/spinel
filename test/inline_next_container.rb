def boxed_block_result(flag)
  values = [:marker]
  values << yield(flag)
  values.last
end

[false, true].each do |flag|
  p boxed_block_result(flag) { |early| next {} if early; {"key" => 7} }
  p boxed_block_result(flag) { |early| next ({}) if early; {1 => "word"} }
  p boxed_block_result(flag) { |early| next [] if early; ["word"] }
  p boxed_block_result(flag) { |early| next [1] if early; ["word"] }
  p boxed_block_result(flag) { |early| next nil if early; {"key" => 7} }
  result = boxed_block_result(flag) do |early|
    begin
      next {} if early
      {"key" => 7}
    ensure
      GC.start
    end
  end
  p result
end

class Hash
  def self.new
    :custom
  end
end
p Hash.new
p boxed_block_result(true) { next {}; {"key" => 7} }
