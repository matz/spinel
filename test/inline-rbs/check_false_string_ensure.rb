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
p result.pick(values[0]), result.pick(values[1]), $cleaned
p result.pick(values[2])
