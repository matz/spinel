module Tagged
  module ClassMethods
    def tag = "tag:#{name}"
  end
  def self.included(base) = base.extend(ClassMethods)
end
