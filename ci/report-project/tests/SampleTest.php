<?php declare(strict_types=1);

namespace Sample\Tests;

use PHPUnit\Framework\TestCase;
use Sample\Sample;

final class SampleTest extends TestCase
{
    private Sample $sut;

    protected function setUp(): void
    {
        $this->sut = new Sample();
    }

    public function testClassify(): void
    {
        $this->assertSame('negative', $this->sut->classify(-3));
        $this->assertSame('zero', $this->sut->classify(0));
        $this->assertSame('positive', $this->sut->classify(7));
    }

    public function testSumEven(): void
    {
        $this->assertSame(6, $this->sut->sumEven([1, 2, 3, 4]));
    }

    public function testLabel(): void
    {
        $this->assertSame('neg', $this->sut->label(-1));
        $this->assertSame('zero', $this->sut->label(0));
        $this->assertSame('pos', $this->sut->label(3));
    }

    public function testSafeDivide(): void
    {
        $this->assertSame('2', $this->sut->safeDivide(4, 2));
        $this->assertSame('error', $this->sut->safeDivide(4, 0));
    }

    public function testLengths(): void
    {
        $this->assertSame([1, 2, 3], $this->sut->lengths(['a', 'bb', 'ccc']));
    }
}
