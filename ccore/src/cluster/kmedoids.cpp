/*!

@authors Andrei Novikov (pyclustering@yandex.ru)
@date 2014-2020
@copyright BSD-3-Clause

*/

#include <pyclustering/cluster/kmedoids.hpp>

#include <algorithm>
#include <limits>
#include <type_traits>

#include <pyclustering/parallel/parallel.hpp>


using namespace pyclustering::parallel;


namespace pyclustering {

namespace clst {


const double             kmedoids::DEFAULT_TOLERANCE         = 0.0001;

const std::size_t        kmedoids::DEFAULT_ITERMAX           = 100;

const std::size_t        kmedoids::OBJECT_ALREADY_CONTAINED  = std::numeric_limits<std::size_t>::max();

const std::size_t        kmedoids::INVALID_INDEX             = std::numeric_limits<std::size_t>::max();

const double             kmedoids::NOTHING_TO_SWAP           = std::numeric_limits<double>::max();


kmedoids::appropriate_cluster::appropriate_cluster(const std::size_t p_index, const double p_distance_first_medoid, const double p_distance_second_medoid) :
    m_index(p_index),
    m_distance_to_first_medoid(p_distance_first_medoid),
    m_distance_to_second_medoid(p_distance_second_medoid)
{ }


kmedoids::kmedoids(const medoid_sequence & p_initial_medoids,
                   const double p_tolerance,
                   const std::size_t p_itermax,
                   const distance_metric<point> & p_metric) :
    m_data_ptr(nullptr),
    m_result_ptr(nullptr),
    m_initial_medoids(p_initial_medoids),
    m_tolerance(p_tolerance),
    m_itermax(p_itermax),
    m_metric(p_metric)
{ }


kmedoids::~kmedoids() { }


void kmedoids::process(const dataset & p_data, kmedoids_data & p_result) {
    process(p_data, data_t::POINTS, p_result);
}


void kmedoids::process(const dataset & p_data, const data_t p_type, kmedoids_data & p_result) {
    m_data_ptr = &p_data;
    m_result_ptr = (kmedoids_data *) &p_result;
    create_distance_calculator(p_type);

    medoid_sequence & medoids = m_result_ptr->medoids();
    medoids.assign(m_initial_medoids.begin(), m_initial_medoids.end());

    m_labels = index_sequence(p_data.size(), -1);
    m_distance_first_medoid = std::vector<double>(p_data.size(), std::numeric_limits<double>::max());
    m_distance_second_medoid = std::vector<double>(p_data.size(), std::numeric_limits<double>::max());

    double changes = std::numeric_limits<double>::max();
    double previous_deviation = std::numeric_limits<double>::max();
    p_result.total_deviation() = 0;

    if (m_itermax > 0) {
        p_result.total_deviation() = update_clusters();
    }

    for (p_result.iterations() = 0; (p_result.iterations() < m_itermax) && (changes > m_tolerance);) {
        p_result.iterations()++;
        const double swap_cost = swap_medoids();

        if (swap_cost != NOTHING_TO_SWAP) {
            previous_deviation = p_result.total_deviation();
            p_result.total_deviation() = update_clusters();
            changes = previous_deviation - p_result.total_deviation();
        }
        else {
            break;
        }
    }

    erase_empty_clusters();

    m_data_ptr = nullptr;
    m_result_ptr = nullptr;
}


double kmedoids::update_clusters() {
    cluster_sequence & clusters = m_result_ptr->clusters();
    medoid_sequence & medoids = m_result_ptr->medoids();

    clusters.clear();
    clusters.resize(medoids.size());

    std::vector<appropriate_cluster> cluster_markers(m_data_ptr->size());
    parallel_for(std::size_t(0), m_data_ptr->size(), [this, &medoids, &cluster_markers](const std::size_t p_index) {
        cluster_markers[p_index] = find_appropriate_cluster(p_index, medoids);
    });

    double total_deviation = 0.0;
    for (std::size_t index_point = 0; index_point < m_data_ptr->size(); index_point++) {
        const std::size_t index_optim = cluster_markers[index_point].m_index;

        total_deviation += cluster_markers[index_point].m_distance_to_first_medoid;

        m_labels[index_point] = index_optim;
        clusters[index_optim].push_back(index_point);

        m_distance_first_medoid[index_point] = cluster_markers[index_point].m_distance_to_first_medoid;
        m_distance_second_medoid[index_point] = cluster_markers[index_point].m_distance_to_second_medoid;
    }

    return total_deviation;
}


double kmedoids::calculate_distance_using_points(const std::size_t p_index1, const std::size_t p_index2) const {
    return m_metric((*m_data_ptr)[p_index1], (*m_data_ptr)[p_index2]);
}


double kmedoids::calculate_distance_using_distance_matrix(const std::size_t p_index1, const std::size_t p_index2) const {
    return (*m_data_ptr)[p_index1][p_index2];
}


void kmedoids::create_distance_calculator(const data_t p_type) {
    if (p_type == data_t::POINTS) {
        m_calculator = std::bind(&kmedoids::calculate_distance_using_points, this, std::placeholders::_1, std::placeholders::_2);
    }
    else if (p_type == data_t::DISTANCE_MATRIX) {
        m_calculator = std::bind(&kmedoids::calculate_distance_using_distance_matrix, this, std::placeholders::_1, std::placeholders::_2);
    }
    else {
        throw std::invalid_argument("Unknown type data is specified (type code: '" + std::to_string(static_cast<std::size_t>(p_type)) + "').");
    }
}


kmedoids::appropriate_cluster kmedoids::find_appropriate_cluster(const std::size_t p_index, medoid_sequence & p_medoids) {
    size_t index_optim = INVALID_INDEX;

    double dist_optim_first = std::numeric_limits<double>::max();
    double dist_optim_second = std::numeric_limits<double>::max();

    for (size_t index = 0; index < p_medoids.size(); index++) {
        const size_t index_medoid = p_medoids[index];
        const double distance = m_calculator(p_index, index_medoid);

        if (distance < dist_optim_first) {
            dist_optim_second = dist_optim_first;
            index_optim = index;
            dist_optim_first = distance;
        }
        else if (distance < dist_optim_second) {
            dist_optim_second = distance;
        }
    }

    return kmedoids::appropriate_cluster(index_optim, dist_optim_first, dist_optim_second);
}


double kmedoids::swap_medoids() {
    double optimal_swap_cost = std::numeric_limits<double>::max();
    std::size_t optimal_index_cluster = INVALID_INDEX;
    std::size_t optimal_index_medoid = INVALID_INDEX;

    auto & medoids = m_result_ptr->medoids();

    struct optimal_chunk {
        double cost = std::numeric_limits<double>::max();
        std::size_t index_medoid = INVALID_INDEX;
    };

    std::vector<optimal_chunk> cluster_chunks(m_result_ptr->clusters().size());
    pyclustering::parallel::parallel_for(std::size_t(0), cluster_chunks.size(), [this, &cluster_chunks, &medoids](std::size_t index_cluster) {
        optimal_chunk & chunk = cluster_chunks[index_cluster];

        for (std::size_t candidate_medoid_index = 0; candidate_medoid_index < m_data_ptr->size(); candidate_medoid_index++) {
            const bool is_already_medoid = std::find(medoids.cbegin(), medoids.cend(), candidate_medoid_index) != medoids.cend();
            if (is_already_medoid || (m_distance_first_medoid[candidate_medoid_index] == 0.0)) {
                continue;
            }

            const double swap_cost = calculate_swap_cost(candidate_medoid_index, index_cluster);
            if (swap_cost < chunk.cost) {
                chunk.cost = swap_cost;
                chunk.index_medoid = candidate_medoid_index;
            }
        }
    });

    for (std::size_t index_cluster = 0; index_cluster < cluster_chunks.size(); ++index_cluster) {
        const optimal_chunk & chunk = cluster_chunks[index_cluster];
        if (chunk.cost < optimal_swap_cost) {
            optimal_swap_cost = chunk.cost;
            optimal_index_cluster = index_cluster;
            optimal_index_medoid = chunk.index_medoid;
        }
    }

    if (optimal_index_cluster != INVALID_INDEX) {
        medoids[optimal_index_cluster] = optimal_index_medoid;
    }

    return optimal_swap_cost;
}


double kmedoids::calculate_swap_cost(const std::size_t p_index_candidate, const std::size_t p_index_cluster) const {
#if PARALLEL_KMEDOIDS_CALCULATE_SWAP_COST
    std::vector<double> point_cost(m_data_ptr->size(), 0);
    pyclustering::parallel::parallel_for(std::size_t(0), m_data_ptr->size(), [this, &p_index_candidate, &p_index_cluster, &point_cost](std::size_t p_index) {
        if (p_index != p_index_candidate) {
            const double candidate_distance = m_calculator(p_index, p_index_candidate);
            if (m_labels[p_index] == p_index_cluster) {
                point_cost[p_index] = std::min(candidate_distance, m_distance_second_medoid[p_index]) - m_distance_first_medoid[p_index];
            }
            else if (candidate_distance < m_distance_first_medoid[p_index]) {
                point_cost[p_index] = candidate_distance - m_distance_first_medoid[p_index];
            }
        }
        });

    const double cost = std::accumulate(point_cost.begin(), point_cost.end(), double(0.0));
    return cost - m_distance_first_medoid[p_index_candidate];
#else
    double cost = 0.0;
    for (std::size_t index_point = 0; index_point < m_data_ptr->size(); ++index_point) {
        if (index_point == p_index_candidate) {
            continue;
        }

        const double candidate_distance = m_calculator(index_point, p_index_candidate);
        if (m_labels[index_point] == p_index_cluster) {
            cost += std::min(candidate_distance, m_distance_second_medoid[index_point]) - m_distance_first_medoid[index_point];
        }
        else if (candidate_distance < m_distance_first_medoid[index_point]) {
            cost += candidate_distance - m_distance_first_medoid[index_point];
        }
    }

    return cost - m_distance_first_medoid[p_index_candidate];
#endif
}


void kmedoids::erase_empty_clusters() {
    auto & clusters = m_result_ptr->clusters();
    auto & medoids = m_result_ptr->medoids();

    for (std::size_t index_cluster = clusters.size() - 1; index_cluster != static_cast<std::size_t>(-1); index_cluster--) {
        if (clusters[index_cluster].empty()) {
            clusters.erase(clusters.begin() + index_cluster);
            medoids.erase(medoids.begin() + index_cluster);
        }
    }
}


}

}
