module Api
  def major = self::VERSION.split(".").first
end

module Basic
  def self.extended(base)
    base.extend(Api)
  end
end
