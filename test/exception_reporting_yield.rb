class GuardedReport < StandardError
  def message
    block_given? ? yield : "fallback"
  end
end

class YieldingReport < StandardError
  def message
    yield
  end
end

p GuardedReport.new.detailed_message
begin
  YieldingReport.new.detailed_message
rescue LocalJumpError => error
  p error.message
end
