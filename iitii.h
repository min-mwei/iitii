/*
Implicit Interval Tree with Interpolation Index (iitii)

This file provides two template classes,
    iit: implicit interval tree, a reimplementation of cgranges by Heng Li
    iitii: iit + interpolation index, experimental extension to speed up queries on very large
           datasets

Both classes take the folllowing four template parameters:
    Pos     :  numeric position type (e.g. uint32_t, double)
    Item    :  arbitrary type of the items to be indexed
    get_beg :  a function (const Item& -> Pos) which accesses the interval begin position of item
    get_end :         "              "                   "                 end          "
For good performance, get_beg and get_end should just access members of Item (cache-local fetch).

Example:

    using intpair = std::pair<int,int>;
    int p_get_beg(const intpair& p) { return p.first; }
    int p_get_end(const intpair& p) { return p.second; }
    using p_iit = iit<int, intpair, p_get_beg, p_get_end>; // first arg is position type

    p_iit::builder br;
    br.add(intpair(12,34));
    br.add(intpair(0,23));
    br.add(intpair(34,56));
    p_iit db = br.build();
    // alternative: p_iit db = p_iit::builder(container.begin(), container.end()).build();

    std::vector<intpair> results = db.overlap(22, 25);
    // alternative: db.overlap(22, 25, results);

Building iitii works the same way, except build() takes a size_t argument giving the number of
model domains.

This header file has other helper template classes that allow most code to be shared between iit
and iitii (without burdening the former with baggage from the latter -- important for fair
comparative benchmarking). The template structure has gotten a little out of hand, which always
seems to happen.
*/

#include <vector>
#include <limits>
#include <algorithm>
#include <cmath>
#include <assert.h>

// Base template for the internal representation of a node within an implicit interval tree
// User should not care about this; subclass instantiations may add more members for more-
// exotically augmented classes of implicit interval trees
template<typename Pos, typename Item, Pos get_beg(const Item&), Pos get_end(const Item&)>
struct iit_node_base {
    static const Pos npos = std::numeric_limits<Pos>::max();  // reserved constant for invalid Pos

    Item item;
    Pos inside_max_end;   // max end of this & subtree (as in textbook augmented interval tree)

    iit_node_base(const Item& item_)
        : item(item_)
        , inside_max_end(get_end(item))
        {}
    inline Pos beg() const {
        return get_beg(item);
    }
    inline Pos end() const {
        return get_end(item);
    }
    bool operator<(const iit_node_base<Pos, Item, get_beg, get_end>& rhs) const {
        auto lbeg = beg(), rbeg = rhs.beg();
        if (lbeg == rbeg) {
            return end() < rhs.end();
        }
        return lbeg < rbeg;
    }
};

// Base template for an implicit interval tree, with internal repr
//     Node<Pos, Item, ...> : iit_node_base<Pos, Item, ...>
// User should not deal with this directly, but instantiate sub-templates iit or iiitii (below)
template<typename Pos, typename Item, typename Node>
class iit_base {
protected:
    // aliases to help keep the Pos, Rank, and Level concepts straight
    typedef std::size_t Rank;   // rank of a node, its index in the sorted array (or beyond, if
                                //                                                imaginary)
    typedef std::size_t Level;  // level in tree

    static const Rank nrank = std::numeric_limits<Rank>::max();  // invalid Rank

    std::vector<Node> nodes;  // array of Nodes sorted by beginning position
    size_t full_size;         // size of the full binary tree containing the nodes; liable to be
                              // as large as 2*nodes.size()-1, including imaginary nodes.

    Rank root;
    Level root_level;         //  = K in cgranges

    // compute a node's level, the # of 1 bits below the lowest 0 bit
    inline Level level(Rank node) const {
        assert(node < full_size);
        Level ans = __builtin_ctzll(~node);  // bitwise-negate & count trailing zeroes
        #ifndef NDEBUG
        Level chk;
        for (chk=0; node&1; chk++, node>>=1);
        assert(ans == chk);
        #endif
        return ans;
    }

    // get node's parent, or nrank if called on the root
    inline Rank parent(Rank node) const {
        assert(node < full_size);
        if (node == root) {
            return nrank;
        }
        Level lv = level(node);
        Rank ofs = Rank(1) << lv;
        assert(node >= ofs-1);
        if (((node>>(lv+1)) & 1)) {  // node is right child
            assert(node >= ofs);
            return node-ofs;
        }
        // node is left child
        return node+ofs;
    }

    // get node's left child, or nrank if called on a leaf
    inline Rank left(Rank node) const {
        Level lv = level(node);
        return lv > 0 ? node - (Rank(1) << (lv-1)) : nrank;
    }

    // get node's right child, or nrank if called on a leaf
    inline Rank right(Rank node) const {
        Level lv = level(node);
        return lv > 0 ? node + (Rank(1) << (lv-1)) : nrank;
    }

    // top-down overlap scan for [qbeg,qend). return # of nodes visited
    // recursion depth limited to tree height
    // TODO: nip&tuck optimizations (eliminate recursion, unroll traversal of low levels, etc.)
    size_t scan(Rank subtree, Pos qbeg, Pos qend, std::vector<Item>& ans) const {
        if (subtree == nrank) {
            return 0;
        }
        assert(subtree < full_size);
        if (subtree >= nodes.size()) {
            // When we arrive at an imaginary node, its right subtree must be all imaginary, so we
            // only need to descend left.
            // TODO: should be able to work out a closed formula for the next real node we'll find,
            //       given subtree, n.size(), and full_size.
            return 1 + scan(left(subtree), qbeg, qend, ans);
        }

        size_t cost = 1;
        const Node& n = nodes[subtree];
        if (n.inside_max_end > qbeg) {  // something in current subtree extends into/over query
            cost += scan(left(subtree), qbeg, qend, ans);
            Pos nbeg = n.beg();
            if (nbeg < qend) {          // this node isn't already past query
                if (n.end() > qbeg) {   // this node overlaps query
                    ans.push_back(n.item);
                }
                cost += scan(right(subtree), qbeg, qend, ans);
            }
        }
        return cost;
    }

public:
    iit_base(std::vector<Node>& nodes_)
        : nodes(std::move(nodes_))
        , root_level(0)
        , root(std::numeric_limits<Rank>::max())
    {
        // compute the implied tree geometry
        for (root_level = 0, full_size = 0; full_size < nodes.size();
             ++root_level, full_size = (size_t(1)<<(root_level+1)) - 1);
        root = (Rank(1) << root_level) - 1;

        if (nodes.size()) {
            // sort the nodes by interval beg (then end)
            std::sort(nodes.begin(), nodes.end());

            // Memoize the path from the rightmost leaf up to the root. This will trace the border
            // between the real and imaginary nodes (if any), which we'll refer to in indexing
            // below. Some of these border nodes may be imaginary.
            std::vector<Rank> right_border_nodes({
                nodes.size() - (2 - nodes.size() % 2)  // rightmost real leaf
            });
            while (right_border_nodes.back() != root) {
                right_border_nodes.push_back(parent(right_border_nodes.back()));
            }

            // bottom-up indexing
            Pos right_border_ime = nodes[right_border_nodes[0]].inside_max_end;
            for (Level lv=1; lv <= root_level; ++lv) {
                // for each in nodes on this level
                size_t x = size_t(1)<<(lv-1), step = x<<2;
                for (Rank n = (x<<1)-1; n < nodes.size(); n += step) {
                    // figure inside_max_end
                    Pos ime = nodes[n].end();
                    ime = std::max(ime, nodes[left(n)].inside_max_end);
                    if (right(n) < nodes.size()) {
                        ime = std::max(ime, nodes[right(n)].inside_max_end);
                    } else {
                        // right child is imaginary; take the last border observation
                        ime = std::max(ime, right_border_ime);
                    }
                    assert(ime != Node::npos);
                    nodes[n].inside_max_end = ime;

                    if (n == right_border_nodes[lv]) {
                        // track inside_max_end of the real nodes on the border
                        right_border_ime = ime;
                    }
                }
            }
        }
    }

    // overlap query; fill ans and return query cost (number of tree nodes visited)
    virtual size_t overlap(Pos qbeg, Pos qend, std::vector<Item>& ans) const {
        ans.clear();
        return scan(root, qbeg, qend, ans);
    }

    // overlap query, return vector of results
    std::vector<Item> overlap(Pos qbeg, Pos qend) const {
        std::vector<Item> ans;
        overlap(qbeg, qend, ans);
        return ans;
    }
};

// template for the builder class exposed by each user-facing class, which takes in items either
// all at once from InputIterator, or streaming one-by-one
template<class iitT, typename Item, typename Node>
class iit_builder_base {
    std::vector<Node> nodes_;

public:
    iit_builder_base() = default;
    template<typename InputIterator>
    iit_builder_base(InputIterator begin, InputIterator end) {
        add(begin, end);
    }

    void add(const Item& it) {
        nodes_.push_back(Node(it));
    }

    template<typename InputIterator>
    void add(InputIterator begin, InputIterator end) {
        std::for_each(begin, end, [&](const Item& it) { add(it); });
    }

    template<typename... Args>
    iitT build(Args&&... args) {
        return iitT(nodes_, std::forward<Args>(args)...);
    }
};

// Basic implicit interval tree (a reimplementation of cgranges)
template<typename Pos, typename Item, Pos get_beg(const Item&), Pos get_end(const Item&)>
class iit : public iit_base<Pos, Item, iit_node_base<Pos, Item, get_beg, get_end>> {
    using Node = iit_node_base<Pos, Item, get_beg, get_end>;

    iit(std::vector<Node>& nodes_)
        : iit_base<Pos, Item, Node>(nodes_)
        {}

public:
    using builder = iit_builder_base<iit<Pos, Item, get_beg, get_end>, Item, Node>;
    friend builder;
};


// iitii-specialized node type
template<typename Pos, typename Item, Pos get_beg(const Item&), Pos get_end(const Item&)>
struct iitii_node : public iit_node_base<Pos, Item, get_beg, get_end> {
    // Additional augment value for iitii nodes, which helps us prove when we can stop climbing in
    // the bottom-up search for a subtree root which must contain all query results beneath it.
    Pos outside_max_end;
    // outside_max_end of node n is the maximum m.end() of all nodes m outside of n & its subtree
    //     with m.beg() < n.beg(); -infinity if there are no such nodes.
    //
    // Furthermore,
    // outside_min_beg of node n is the minimum m.beg() of all nodes m outside of n & its subtree
    //     with m.beg() >= n.beg(); infinity if there are no such nodes.
    // But we don't need to store outside_min_beg, because we can compute it in constant time using
    // rank offsets in the beg-sorted node array.
    //
    // Suppose during a query for [qbeg, qend) we climb up to a node n with,
    //   (i) n.outside_max_end <= qbeg; AND
    //  (ii) qend <= n.outside_min_beg
    // By (i), any node outside n & subtree with beg < n's cannot overlap the query. By (ii), any
    // node outside n & subtree with beg >= n's cannot overlap the query. This exhausts all nodes
    // outside of n & subtree, so we need not climb past n.

    iitii_node(const Item& item_)
        : iit_node_base<Pos, Item, get_beg, get_end>(item_)
        , outside_max_end(std::numeric_limits<Pos>::min())
        {}
};

// simple linear regression of y ~ x given points [(x,y)], returning (intercept, slope)
template<typename XT, typename YT>
std::pair<double,double> regress(const std::vector<std::pair<XT,YT>>& points) {
    if (points.empty()) {
        return std::make_pair(std::numeric_limits<double>::quiet_NaN(),
                              std::numeric_limits<double>::quiet_NaN());
    }
    double sum_x, sum_y, cov, var;
    sum_x = sum_y = cov = var = 0.0;
    for (const auto& pt : points) {
        sum_x += double(pt.first);
        sum_y += double(pt.second);
    }
    const double mean_x = sum_x/points.size(), mean_y = sum_y/points.size();
    for (const auto& pt : points) {
        const double x_err = pt.first - mean_x;
        cov += x_err*(pt.second - mean_y);
        var += x_err*x_err;
    }
    if (var == 0.0) {
        return std::make_pair(0.0, 0.0);
    }
    const double m = cov / var;
    return std::make_pair(mean_y - m*mean_x, m);
}

template<typename XT, typename YT>
double mean_absolute_residual(const std::vector<std::pair<XT,YT>>& points, double b, double m) {
    if (points.empty()) {
        return std::numeric_limits<double>::quiet_NaN();
    }
    double sr = 0.0;
    for (const auto& pt : points) {
        double y = double(pt.second), fx = (m*double(pt.first)+b);
        sr += y >= fx ? y-fx : fx-y;
    }
    return sr/points.size();
}

// here it is
template<typename Pos, typename Item, Pos get_beg(const Item&), Pos get_end(const Item&)>
class iitii : public iit_base<Pos, Item, iitii_node<Pos, Item, get_beg, get_end>> {
    using Node = iitii_node<Pos, Item, get_beg, get_end>;
    using super = iit_base<Pos, Item, Node>;
    using typename super::Rank;
    using typename super::Level;
    using super::left;
    using super::right;
    using super::parent;
    using super::nodes;
    using super::full_size;
    using super::nrank;
    using super::level;
    using super::root;
    using super::root_level;


    // Rank prediction model: the (max_beg-min_beg) range is partitioned into a number of domains
    // C, each domain covering an equal-sized portion of that range. The domain pertaining to a
    // position beg is d(beg) = floor((beg-min_beg)*C/(max_beg-min_beg)), bounded to [0,C).
    //
    // For each domain d, we store three parameters: a Level l[d] ∈ [0,root_level] into which we
    // will jump, and linear weights w[d,0] and w[d,1] for the regression of the offset within
    // level l[d] on position:
    //   ofs_l(beg) ~ w[d(beg),0] + w[d(beg),1]*beg
    // So ofs_l = 0 refers to the leftmost node on level l[d], ofs_l = 1 refers to the node to its
    // right on level l[d] and so on.
    //
    // Given the (rounded and bounded) offset, the rank of the corresponding node in the array is:
    //   rank(beg) = 2^{l[d(beg)]}*(2*ofs_l(beg)+1)-1

    unsigned domains; // C
    Pos domain_size = Node::npos;
    std::vector<float> parameters;  // C rows of three parameters (row-major storage): w[0,d],
                                    // w[1,d] and l[d]. NB: the third is a Level stored as a float.
    Pos min_beg = std::numeric_limits<Pos>::max();

    inline unsigned which_domain(Pos beg) const {
        if (beg < min_beg) {
            return 0;
        }
        return std::min(domains-1, unsigned((beg-min_beg)/domain_size));
    }

    // Given qbeg, select domain and predict rank from the respective model
    Rank predict_leaf(Pos qbeg) const {
        auto which = which_domain(qbeg);
        assert(which < domains);

        const float lv_f = parameters[3*which+2];
        if (!std::isfinite(lv_f)) {
            return nrank;
        }
        assert(lv_f >= 0 && lv_f <= root_level);
        const Level lv = (Level) lv_f;

        const float ofs_f = parameters[3*which] + parameters[3*which+1]*float(qbeg);
        assert(std::isfinite(ofs_f));
        const size_t ofs = size_t(std::max(0.0f, roundf(ofs_f)));

        Rank r = (size_t(1)<<lv)*(2*ofs+1)-1;
        assert(r >= nodes.size() || level(r) == lv);

        // detail: if rank is imaginary (off-scale high), start from rightmost real leaf
        const auto nsz = nodes.size();
        return r < nsz ? r : (nsz - (2 - nsz%2));
    }

    void train(Level lv = 0) {
        std::vector<std::vector<std::pair<Pos,Rank>>> points;
        points.resize(domains);
        // partition all the nodes into their respective domains & record training points
        const size_t step = size_t(1)<<(lv+1);
        for (Rank r = (size_t(1)<<lv)-1, ofs = 0; r < nodes.size(); r+=step, ++ofs) {
            assert(level(r) == lv);
            points.at(which_domain(nodes[r].beg()))
                    .push_back(std::make_pair(nodes[r].beg(), ofs));
        }
        // train each domain-specific model
        for (unsigned which = 0; which < domains; ++which) {
            auto w = regress<Pos,Rank>(points[which]);
            // Keep the model if the regression succeeded and the mean absolute residual from the
            // training points is <= 2^(half of tree height). Otherwise we consider the bottom-up
            // search counterproductive; leaving the respective weights at nan will cause overlap()
            // to fall back to top-down search from the root.
            if (std::isfinite(w.first) && std::isfinite(w.second) &&
                mean_absolute_residual(points[which], w.first, w.second) <= double(1 << (root_level/2))) {
                parameters[3*which] = float(w.first);
                parameters[3*which+1] = float(w.second);
                parameters[3*which+2] = float(lv);
            }
            // std::cout << "which = " << which << " points = " << points[which].size()
            //           << " b = " << w.first << " m = " << w.second << std::endl;
        }
    }

    inline Rank leftmost_child(Rank subtree) const {
        Level k = level(subtree);
        auto ofs = (Rank(1)<<k) - 1;
        assert(subtree >= ofs);
        return subtree - ofs;
    }

    inline Rank rightmost_child(Rank subtree) const {
        Level k = level(subtree);
        auto ofs = (Rank(1)<<k) - 1;
        assert(subtree + ofs < full_size);
        return subtree + ofs;
    }

    inline Pos outside_min_beg(Rank subtree) const {
        // constant-time computation of outside_min_beg: beg() of the node ranked one higher than
        // subtree's rightmost child
        const Pos beg = nodes[subtree].beg();
        const Rank l = leftmost_child(subtree);
        if (l && nodes[l-1].beg() == beg) {
            // corner case: nodes to the left of the subtree can have the same beg as subroot
            // and outside_min_beg is defined on nodes with beg >= subroot's.
            return beg;
        }
        const Rank r = rightmost_child(subtree);
        return r < nodes.size()-1 ? nodes[r+1].beg() : std::numeric_limits<Pos>::max();
    }

    iitii(std::vector<Node>& nodes_, unsigned domains_)
        : super(nodes_)
        , domains(std::max(1U,domains_))
        , domain_size(std::numeric_limits<Pos>::max())
    {
        parameters.resize(domains*3, std::numeric_limits<float>::quiet_NaN());

        if (nodes.size()) {
            // equal size (in Pos units) of each domain
            min_beg = nodes[0].beg();
            domain_size = 1 + (nodes.back().beg()-min_beg)/domains;

            // compute running max_end along the sorted array, which we'll look up while computing
            // outside_max_end below
            std::vector<Pos> running_max_end { nodes[0].end() };
            for (Rank n = 1; n < nodes.size(); ++n) {
                running_max_end.push_back(std::max(running_max_end[n-1], nodes[n].end()));
            }

            // fill outside_max_end
            for (Rank n = 0; n < nodes.size(); ++n) {
                Node& node = nodes[n];
                Rank l = leftmost_child(n);

                if (l>0) {
                    // outside_max_end is the running_max_end of the highest-ranked node ranked
                    // below n's leftmost child & has beg strictly below n's
                    Rank leq = l-1;
                    while (nodes[leq].beg() == node.beg()) {
                        if (leq == 0) {
                            break;
                        }
                        --leq;
                    }
                    assert(nodes[leq].beg() <= node.beg());
                    node.outside_max_end = nodes[leq].beg() < node.beg()
                                                ? running_max_end[leq]
                                                : std::numeric_limits<Pos>::min();
                }
            }

            // train the rank prediction models
            train();
        }
    }

public:
    // iitii::builder::build() takes a size_t argument giving the number of domains to model
    using builder = iit_builder_base<iitii<Pos, Item, get_beg, get_end>, Item, Node>;
    friend builder;

    size_t overlap(Pos qbeg, Pos qend, std::vector<Item>& ans) const override {
        // ask model which leaf we should begin our bottom-up climb at
        Rank prediction = predict_leaf(qbeg);
        if (prediction == nrank) {
            // the model did not make a prediction for some reason, so just go to the root
            return super::overlap(qbeg, qend, ans);
        }
        assert(level(prediction) == 0);

        // climb until our necessary & sufficient criteria are met, or the root
        size_t climb_cost = 0;
        Rank subtree = prediction;
        while (subtree != root &&                           // stop at root
                (subtree >= nodes.size() ||                 // continue climb through imaginary
                 qbeg < nodes[subtree].outside_max_end ||   // possible outside overlap from left
                 outside_min_beg(subtree) < qend)) {        // possible outside overlap from right
            subtree = parent(subtree);
            // TODO: scheme to skip through chains of imaginary nodes along the border
            ++climb_cost;
        }

        auto self = const_cast<iitii<Pos, Item, get_beg, get_end>*>(this);  // getting around const
        self->queries++;
        self->total_climb_cost += climb_cost;

        // scan the subtree for query results 
        ans.clear();
        return super::scan(subtree, qbeg, qend, ans) + climb_cost;
    }

    size_t queries = 0;
    size_t total_climb_cost = 0;

    using super::overlap;
};
