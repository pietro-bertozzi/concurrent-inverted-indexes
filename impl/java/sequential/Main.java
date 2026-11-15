/** Java sequential variant: one worker thread, one HashMap, no locks. */
public final class Main {
    public static void main(String[] args) {
        Runner.run(args, 1, a -> new SequentialIndex());
    }
}
