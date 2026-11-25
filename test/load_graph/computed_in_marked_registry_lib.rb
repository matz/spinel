module Registry
  @paths = {}
  def self.register(name, path) = @paths[name] = path
  def self.load_all!
    @paths.each { |_, path| require(path) }
  end
end
