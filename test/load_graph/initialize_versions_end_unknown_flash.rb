class Flash
  def initialize
    @notice = nil
  end

  def []=(key, value)
    @notice = value
  end
end
