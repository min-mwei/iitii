# iitii
**Implicit Interval Tree with Interpolation Index**

iitii is a data structure for indexing begin/end position intervals, such as genomic feature annotations, and answering requests for all items overlapping a query interval. Building on [cgranges by Heng Li](https://github.com/lh3/cgranges), iitii explores ideas from [DBMS interpolation search](https://www.cs.cmu.edu/~damon2006/pdf/graefe06btreeindexes.pdf) and [learned index structures](https://arxiv.org/abs/1712.01208) to speed up queries on large datasets.

We have [notes on cgranges](https://github.com/mlin/iitii/blob/master/notes_on_cgranges.md) for background; briefly, it stores the items in a flat array, sorted by interval begin position, and interprets the array as an implicit [augmented interval tree](https://en.wikipedia.org/wiki/Interval_tree#Augmented_tree). Following a textbook algorithm, queries descend through the tree levels and narrow down a subtree which must contain the result set. If (i) most of the indexed intervals are small, (ii) most queries are small intervals, and (iii) neither distribute too pathologically, then the relevant subtrees tend to be small & near the leaves. The top-down search spends most of ~log<sub>2</sub>(*N*) iterations wading through higher tree levels.

**iitii shortcuts the top-down search by starting from an internal node** (or leaf) whose begin position is near the query's. From this starting node, climb *up* its parents as needed to find a sufficient subtree root, then execute the top-down search from there. For small query intervals this "middle-out" search often traverses substantially fewer than log<sub>2</sub>(*N*) nodes with improved cache-locality. An extra "augmentation" value in each node helps prove subtree sufficiency whilst climbing from below.

But don't we first have to search for a suitable starting node, defeating the point? Not if it's practically sufficient to **guess** one quickly. An accurate guess helps narrow down the result set, while a poor guess still leads to correct results in O(log<sub>2</sub>(*N*)) steps (plus result set size); for if the guess is horrible, we effectively just climb up to the root and search down from there.

How to guess the start node? iitii's indexing procedure includes a simple linear regression of each node's rank within its tree level on its interval begin position. Given a query interval [qbeg, qend), we can use the model prediction (rank ~ w·qbeg+w<sub>0</sub>) to select a node at which to begin, and then jump to the corresponding index in the sorted array. **If the model is accurate and (i-iii) above hold, then iitii queries enjoy average-case speedup over the textbook algorithm**.

To improve the rank prediction, iitii can also partition the begin position range into a fixed number of domains, each with its own preferred starting level and regression model; and apply the domain-specific model when presented with a query. The starting level is chosen while indexing each domain, by minimizing estimated search cost based on the regression accuracy on each level and the tendency of domain items to overlap. These elaborations help when item density and/or overlap tendency fluctuate along the position range, for example varying with regional GC% along a chromosome.

If the data distribution is too challenging for this piecewise-linear model, in principle one could switch to any nonlinear regression method; even so far as neural nets, as explored in [*The Case for Learned Index Structures*](https://arxiv.org/abs/1712.01208), if the accuracy offsets the cost of prediction to start each query. Further to the related work already mentioned, iitii takes inspiration from [SAPLING](https://www.cs.ucf.edu/stringbio2018/talks/talk11.pdf) by Michael Kirsche & Michael Schatz, which deploys similar ideas on suffix array lookup. Other widely-used interval index structures might benefit from interpolation techniques. In broad strokes, seeking to an approximate location in sorted intervals recalls the binning scheme employed in [UCSC Genome Browser](https://www.ncbi.nlm.nih.gov/pmc/articles/PMC186604/figure/F7/) and [tabix](https://www.ncbi.nlm.nih.gov/pmc/articles/PMC3042176/).

### Usage

```cpp
// g++ -std=c++11 -o example example.cc
#include <iostream>
#include <vector>
#include "iitii.h"

using intpair = std::pair<int,int>;
int p_get_beg(const intpair& p) { return p.first; }
int p_get_end(const intpair& p) { return p.second; }
using p_iitii = iitii<int, intpair, p_get_beg, p_get_end>;  // first arg is position type

int main() {
    p_iitii::builder br;
    br.add(intpair(12,34));
    br.add(intpair(0,23));
    br.add(intpair(34,56));
    p_iitii db = br.build(1);  // 1 = # domains
    // alternative: p_iitii db = p_iitii::builder(container.begin(), container.end()).build(1);

    std::vector<intpair> results = db.overlap(22, 25);
    // alternative: db.overlap(22, 25, results);

    for (const auto& p : results)
        std::cout << p.first << "\t" << p.second << std::endl;
    return 0;
}
```

### Experiment: synthetic ideal

First we tested iitii using intervals whose positions are predicted perfectly by the linear model: each node ranked *r* has begin position 10*r*, with geometrically-distributed length, mean 20. We measured the time to run 10 million queries (uniform random begin, geometric length mean 10) using iitii and iit, the included vanilla reimplementation of cgranges.

![image](https://user-images.githubusercontent.com/356550/62170011-c343e000-b2de-11e9-9598-882c8f808c73.png)

iiti yields a huge advantage in this ideal setting, growing along with the number *N* of items indexed. Some of iitii's slope is probably because of my computer's thermal problems.

### Experiment: gnomAD variants

Next, we used the [gnomAD chr2 sites VCF file](https://gnomad.broadinstitute.org/downloads), with about 20 million variant sites.  We loaded the variants from varying-sized segments of the chromosome into iit and iitii with varying numbers of model domains. Against each structure we perform 10 million queries, 50% for the interval of one of the indexed variants picked at random, and the other 50% for 10bp intervals with uniform random begin positions in the relevant range.

![image](https://user-images.githubusercontent.com/356550/62170024-cdfe7500-b2de-11e9-9574-2c736876779c.png)

iitii starts to yield substantial gains using a few hundred domains (256 domains is roughly one per Mbp of the full chromosome, while 65,536 is roughly one per 4Kbp). Within reason, using a lot of domains doesn't cost much time: during indexing, each models only its proportionate partition of the data, and during query, the appropriate parameters are looked up by offset. We can see a certain over-fitting effect for the smaller datasets, wherein the 65,536 domains tend to be too sparse.

This gnomAD VCF is probably as close to ideal for iitii as real genomic data will get: the intervals are short, with limited overlaps, dispersed quite regularly throughout the chromosome. At this early stage there's still low-hanging fruit for substantial improvement even in this setting, and much further research can be envisioned for deploying these techniques on not-so-nicely-behaved datasets.
