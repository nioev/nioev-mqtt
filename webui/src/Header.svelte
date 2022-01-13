<script>
    let currentPage = window.location.pathname.split("/").pop();

    let navElements = [{
        href: "/index.html",
        text: "Overview"
    }, {
        href: "/stats.html",
        text: "Stats"
    }, {
        href: "/scripts.html",
        text: "Scripts"
    }, {
        href: "/settings.html",
        text: "Settings"
    }]
</script>

<main>
    <div id="blueBar"></div>
    <div id="innerMain">
        <h1>
            nioev live dashboard
        </h1>
        <nav>
            {#each navElements as e}
                {#if "/" + currentPage === e.href}
                    <a href={e.href} class="element" style="font-weight: bold">
                        {e.text}
                    </a>
                {:else}
                    <a href={e.href} class="element">
                        {e.text}
                    </a>
                {/if}
            {/each}
        </nav>
    </div>
</main>

<style>
    h1 {
        margin: 0;
        color: white;
    }
    #blueBar {
        background-color: #2980b9;
        height: 150px;
        transform: translateY(-20px);
        z-index: -1;
        width: 100%;
    }
    main {
        width: 100%;
        margin: 0;
        height: 100px;
        display: grid;
        grid-template-rows: 50px 100px;
        justify-items: center;
    }
    nav {
        display: flex;
        flex-direction: row;
        background-color: white;
        border-radius: 10px;
        box-shadow: #333333 0 0 5px;
        align-items: center;
    }
    #innerMain {
        display: grid;
        grid-template-columns: 100%;
        grid-template-rows: 50px 50px;
        width: 1000px;
    }
    @media only screen and (max-width: 1111px) {
        #innerMain {
            width: 90%;
        }
    }
    .element {
        text-align: center;
        padding-left: 20px;
        padding-right: 20px;
        font-size: 20px;
        color: black;
    }
</style>