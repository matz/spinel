# spinel: not-cruby -- a false return annotation aborts under -DSP_RBS_CHECK
class CheckedResult
  #: (untyped) -> String?
  def pick(value)
    begin
      return value
    ensure
      $cleaned = true
    end
  end
end

values = ["honest", nil, 17]
result = CheckedResult.new
if ARGV.empty?
  p result.pick(values[2])
else
  p result.pick(values[0]), result.pick(values[1]), $cleaned
end
