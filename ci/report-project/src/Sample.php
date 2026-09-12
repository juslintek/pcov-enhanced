<?php declare(strict_types=1);

namespace Sample;

/**
 * Exercises the constructs branch/path coverage cares about: conditionals,
 * loops, match, try/catch/finally, and a first-class callable.
 */
final class Sample
{
    public function classify(int $n): string
    {
        if ($n < 0) {
            return 'negative';
        } elseif ($n === 0) {
            return 'zero';
        }

        return 'positive';
    }

    public function sumEven(array $numbers): int
    {
        $sum = 0;

        foreach ($numbers as $number) {
            if ($number % 2 === 0) {
                $sum += $number;
            }
        }

        return $sum;
    }

    public function label(int $n): string
    {
        return match (true) {
            $n < 0   => 'neg',
            $n === 0 => 'zero',
            default  => 'pos',
        };
    }

    public function safeDivide(int $a, int $b): string
    {
        try {
            if ($b === 0) {
                throw new \DivisionByZeroError('division by zero');
            }

            return (string) intdiv($a, $b);
        } catch (\DivisionByZeroError $e) {
            return 'error';
        } finally {
            // finally block participates in path coverage
            $marker = true;
        }
    }

    public function lengths(array $words): array
    {
        return array_map(strlen(...), $words);
    }
}
