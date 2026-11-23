class Flash
  def initialize
    @notice = nil
    @discard = []
  end

  def []=(key, value)
    @discard.delete(key)
    @notice = value
  end

  def discard = @discard
end
