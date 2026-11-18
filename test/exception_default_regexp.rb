class ReportError < StandardError
  def message(pattern = /hook_only/)
    pattern.source
  end
end

p ReportError.new("seed").detailed_message
